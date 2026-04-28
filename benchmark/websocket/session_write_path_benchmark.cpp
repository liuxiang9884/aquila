#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/frame_codec.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>
#include <endian.h>
#include <openssl/rand.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

class OneWritePerDriveSocket {
 public:
  void ResetWriteBudget() noexcept { write_available_ = true; }

  ssize_t ReadSome(std::span<std::byte>) noexcept {
    errno = EAGAIN;
    return -1;
  }

  size_t PendingReadableBytes() const noexcept { return 0; }

  ssize_t WriteSome(std::span<const std::byte> buffer) noexcept {
    if (!write_available_) {
      errno = EAGAIN;
      return -1;
    }
    write_available_ = false;
    bytes_written_ += buffer.size();
    return static_cast<ssize_t>(buffer.size());
  }

  size_t bytes_written() const noexcept { return bytes_written_; }

 private:
  bool write_available_{true};
  size_t bytes_written_{0};
};

class RawSocketPair {
 public:
  RawSocketPair() noexcept {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      return;
    }
    if (!SetNonBlocking(fds[0]) || !SetNonBlocking(fds[1])) {
      ::close(fds[0]);
      ::close(fds[1]);
      return;
    }
    client_fd_ = fds[0];
    peer_fd_ = fds[1];
  }

  ~RawSocketPair() noexcept {
    if (client_fd_ >= 0) {
      ::close(client_fd_);
    }
    if (peer_fd_ >= 0) {
      ::close(peer_fd_);
    }
  }

  RawSocketPair(const RawSocketPair&) = delete;
  RawSocketPair& operator=(const RawSocketPair&) = delete;

  bool valid() const noexcept { return client_fd_ >= 0 && peer_fd_ >= 0; }
  int client_fd() const noexcept { return client_fd_; }
  int peer_fd() const noexcept { return peer_fd_; }

 private:
  int client_fd_{-1};
  int peer_fd_{-1};
};

std::vector<std::byte> BuildWritePayload(size_t payload_size = 64) {
  std::vector<std::byte> payload(payload_size);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = std::byte{static_cast<unsigned char>(i & 0xFFU)};
  }
  return payload;
}

size_t ClientFrameWireBytes(size_t payload_size) noexcept {
  const size_t extended_length_bytes =
      payload_size <= 125 ? 0 : (payload_size <= 0xFFFF ? 2 : 8);
  return 2 + extended_length_bytes + 4 + payload_size;
}

size_t PreparedWriteBytesForPayload(size_t payload_size) noexcept {
  return std::max<size_t>(128, ClientFrameWireBytes(payload_size));
}

class DrogonStyleMaskCache {
 public:
  bool Next(std::uint32_t* mask) noexcept {
    if (mask == nullptr) {
      return false;
    }
    if (!using_mask_.exchange(true, std::memory_order_acq_rel)) {
      if (masks_.empty() && !Refill()) {
        using_mask_.store(false, std::memory_order_release);
        return false;
      }
      *mask = masks_.back();
      masks_.pop_back();
      using_mask_.store(false, std::memory_order_release);
      return true;
    }
    return RAND_bytes(reinterpret_cast<unsigned char*>(mask),
                      static_cast<int>(sizeof(*mask))) == 1;
  }

 private:
  bool Refill() {
    masks_.resize(16);
    return RAND_bytes(reinterpret_cast<unsigned char*>(masks_.data()),
                      static_cast<int>(masks_.size() * sizeof(masks_[0]))) == 1;
  }

  std::vector<std::uint32_t> masks_{};
  std::atomic<bool> using_mask_{false};
};

bool EncodeDrogonStyleClientFrame(std::span<const std::byte> payload,
                                  DrogonStyleMaskCache& mask_cache,
                                  std::string* output) {
  if (output == nullptr) {
    return false;
  }

  const std::uint64_t len = payload.size();
  output->resize(static_cast<size_t>(len + 10U));
  (*output)[0] = static_cast<char>(0x82);

  int index_start_raw_data = -1;
  if (len <= 125) {
    (*output)[1] = static_cast<char>(len);
    index_start_raw_data = 2;
  } else if (len <= 65535) {
    (*output)[1] = static_cast<char>(126);
    (*output)[2] = static_cast<char>((len >> 8U) & 0xFFU);
    (*output)[3] = static_cast<char>(len & 0xFFU);
    index_start_raw_data = 4;
  } else {
    (*output)[1] = static_cast<char>(127);
    for (int shift = 56, index = 2; shift >= 0; shift -= 8, ++index) {
      (*output)[index] = static_cast<char>((len >> shift) & 0xFFU);
    }
    index_start_raw_data = 10;
  }

  std::uint32_t mask = 0;
  if (!mask_cache.Next(&mask)) {
    return false;
  }

  (*output)[1] = static_cast<char>((*output)[1] | 0x80);
  output->resize(static_cast<size_t>(index_start_raw_data) + sizeof(mask) +
                 payload.size());
  std::memcpy(output->data() + index_start_raw_data, &mask, sizeof(mask));
  for (size_t i = 0; i < payload.size(); ++i) {
    (*output)[static_cast<size_t>(index_start_raw_data) + sizeof(mask) + i] =
        static_cast<char>(
            payload[i] ^
            static_cast<std::byte>(
                (*output)[static_cast<size_t>(index_start_raw_data) +
                          (i & 0x3U)]));
  }
  return true;
}

bool ThirdPartyStyleWriteAll(int fd, const std::uint8_t* data,
                             std::uint32_t size, bool more) noexcept {
  int flags = MSG_NOSIGNAL;
  if (more) {
    flags |= MSG_MORE;
  }
  do {
    const ssize_t sent = ::send(fd, data, size, flags);
    if (sent < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return false;
      }
      continue;
    }
    if (sent == 0) {
      return false;
    }
    data += sent;
    size -= static_cast<std::uint32_t>(sent);
  } while (size != 0);
  return true;
}

bool ThirdPartyStyleSendBinary(int fd,
                               std::span<const std::byte> payload) noexcept {
  std::uint8_t header[14]{};
  std::uint32_t header_len = 2;
  constexpr std::uint8_t kOpcodeBinary = 2;
  constexpr bool kFin = true;
  constexpr bool kSendMask = true;
  const auto payload_len = static_cast<std::uint32_t>(payload.size());

  header[0] = (kOpcodeBinary & 15U) | (static_cast<std::uint8_t>(kFin) << 7U);
  header[1] = static_cast<std::uint8_t>(kSendMask) << 7U;
  if (payload_len < 126) {
    header[1] |= static_cast<std::uint8_t>(payload_len);
  } else if (payload_len < 65536) {
    header[1] |= 126;
    const std::uint16_t be_len =
        htobe16(static_cast<std::uint16_t>(payload_len));
    std::memcpy(header + 2, &be_len, sizeof(be_len));
    header_len += 2;
  } else {
    header[1] |= 127;
    const std::uint64_t be_len = htobe64(payload_len);
    std::memcpy(header + 2, &be_len, sizeof(be_len));
    header_len += 8;
  }

  if constexpr (kSendMask) {
    const std::uint32_t zero_mask = 0;
    std::memcpy(header + header_len, &zero_mask, sizeof(zero_mask));
    header_len += sizeof(zero_mask);
  }

  const auto* payload_data =
      reinterpret_cast<const std::uint8_t*>(payload.data());
  return ThirdPartyStyleWriteAll(fd, header, header_len, true) &&
         ThirdPartyStyleWriteAll(fd, payload_data, payload_len, false);
}

void BenchmarkSessionWritePath(benchmark::State& state) {
  ConnectionConfig config{};
  config.enable_tls = false;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 128;
  config.read_buffer_bytes = 4096;
  config.frame_buffer_bytes = 4096;
  config.heartbeat_interval_ms = std::numeric_limits<std::uint32_t>::max();
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();

  SocketPair pair;
  if (!CreateSocketPair(pair)) {
    state.SkipWithError("socketpair create failed");
    return;
  }

  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  CriticalSession<LocalFdSocket> session(config, pair.client, arena, metrics);
  const auto payload = BuildWritePayload();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::array<std::byte, 64> peer_drain{};

  for (auto _ : state) {
    state.PauseTiming();
    PreparedWrite* write = session.TryAcquirePreparedWrite();
    if (write == nullptr) {
      state.SkipWithError("prepared write slot unavailable");
      return;
    }
    std::copy(payload.begin(), payload.end(), write->storage.begin());
    write->encoded_size = static_cast<std::uint32_t>(payload.size());
    write->kind = PayloadKind::kBinary;
    state.ResumeTiming();

    const std::uint64_t start_ns = NowNs();
    if (session.CommitPreparedWrite(write) != SendStatus::kOk) {
      state.SkipWithError("commit prepared write failed");
      return;
    }
    while (session.WantsWrite()) {
      session.DriveWrite();
      if (session.ShouldReconnect()) {
        state.SkipWithError("session requested reconnect");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);

    state.PauseTiming();
    if (!ReadExactFd(pair.peer_fd, peer_drain)) {
      state.SkipWithError("peer drain failed");
      return;
    }
    state.ResumeTiming();
  }

  SetLatencyCounters(state, std::move(samples_ns), "tx_messages",
                     metrics.tx_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "local-socketpair",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkSessionWritePath)
    ->Name("session_write_path")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

void RunSessionWritePathWithEncode(benchmark::State& state,
                                   size_t payload_size) {
  ConnectionConfig config{};
  config.enable_tls = false;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = PreparedWriteBytesForPayload(payload_size);
  config.read_buffer_bytes = 4096;
  config.frame_buffer_bytes = 4096;
  config.heartbeat_interval_ms = std::numeric_limits<std::uint32_t>::max();
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();

  SocketPair pair;
  if (!CreateSocketPair(pair)) {
    state.SkipWithError("socketpair create failed");
    return;
  }

  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  CriticalSession<LocalFdSocket> session(config, pair.client, arena, metrics);
  FrameCodec encoder(config.max_frame_payload_bytes, config.frame_buffer_bytes);
  const auto payload = BuildWritePayload(payload_size);
  const auto payload_bytes = std::span<const std::byte>(payload.data(),
                                                       payload.size());
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::vector<std::byte> peer_drain(ClientFrameWireBytes(payload_size));

  for (auto _ : state) {
    state.PauseTiming();
    PreparedWrite* write = session.TryAcquirePreparedWrite();
    if (write == nullptr) {
      state.SkipWithError("prepared write slot unavailable");
      return;
    }
    state.ResumeTiming();

    const std::uint64_t start_ns = NowNs();
    const auto encoded = encoder.EncodeBinary(payload_bytes, write->storage);
    if (!encoded.ok) {
      state.SkipWithError("frame encode failed");
      return;
    }
    write->encoded_size = static_cast<std::uint32_t>(encoded.bytes.size());
    write->write_offset = 0;
    write->kind = PayloadKind::kBinary;
    if (session.CommitPreparedWrite(write) != SendStatus::kOk) {
      state.SkipWithError("commit prepared write failed");
      return;
    }
    while (session.WantsWrite()) {
      session.DriveWrite();
      if (session.ShouldReconnect()) {
        state.SkipWithError("session requested reconnect");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);

    state.PauseTiming();
    if (!ReadExactFd(pair.peer_fd,
                     std::span<std::byte>(peer_drain.data(),
                                          encoded.bytes.size()))) {
      state.SkipWithError("peer drain failed");
      return;
    }
    state.ResumeTiming();
  }

  SetLatencyCounters(state, std::move(samples_ns), "tx_messages",
                     metrics.tx_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "local-socketpair-with-encode",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkSessionWritePathWithEncode(benchmark::State& state) {
  RunSessionWritePathWithEncode(state, 64);
}

BENCHMARK(BenchmarkSessionWritePathWithEncode)
    ->Name("session_write_path_with_encode_plain")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

void BenchmarkSessionWritePathWithEncode256(benchmark::State& state) {
  RunSessionWritePathWithEncode(state, 256);
}

void BenchmarkSessionWritePathWithEncode1024(benchmark::State& state) {
  RunSessionWritePathWithEncode(state, 1024);
}

void BenchmarkSessionWritePathWithEncode4096(benchmark::State& state) {
  RunSessionWritePathWithEncode(state, 4096);
}

BENCHMARK(BenchmarkSessionWritePathWithEncode256)
    ->Name("session_write_path_with_encode_plain_256")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkSessionWritePathWithEncode1024)
    ->Name("session_write_path_with_encode_plain_1024")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkSessionWritePathWithEncode4096)
    ->Name("session_write_path_with_encode_plain_4096")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

void RunDrogonStyleWritePathPlain(benchmark::State& state,
                                  size_t payload_size) {
  SocketPair pair;
  if (!CreateSocketPair(pair)) {
    state.SkipWithError("socketpair create failed");
    return;
  }

  DrogonStyleMaskCache mask_cache;
  const auto payload = BuildWritePayload(payload_size);
  const auto payload_bytes = std::span<const std::byte>(payload.data(),
                                                       payload.size());
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::vector<std::byte> peer_drain(ClientFrameWireBytes(payload_size));
  std::uint64_t frames_sent = 0;

  for (auto _ : state) {
    size_t frame_bytes = 0;
    const std::uint64_t start_ns = NowNs();
    std::string frame;
    if (!EncodeDrogonStyleClientFrame(payload_bytes, mask_cache, &frame)) {
      state.SkipWithError("drogon-style frame encode failed");
      return;
    }
    const auto frame_bytes_span =
        std::as_bytes(std::span<const char>(frame.data(), frame.size()));
    const ssize_t written = pair.client.WriteSome(frame_bytes_span);
    if (written != static_cast<ssize_t>(frame_bytes_span.size())) {
      state.SkipWithError("drogon-style write failed");
      return;
    }
    frame_bytes = frame.size();
    ++frames_sent;
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);

    state.PauseTiming();
    if (!ReadExactFd(pair.peer_fd,
                     std::span<std::byte>(peer_drain.data(), frame_bytes))) {
      state.SkipWithError("peer drain failed");
      return;
    }
    state.ResumeTiming();
  }

  SetLatencyCounters(state, std::move(samples_ns), "frames_sent", frames_sent);
  state.SetLabel(BuildBenchmarkLabel(false, "drogon-style-local-socketpair",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkDrogonStyleWritePathPlain(benchmark::State& state) {
  RunDrogonStyleWritePathPlain(state, 64);
}

BENCHMARK(BenchmarkDrogonStyleWritePathPlain)
    ->Name("drogon_style_write_path_plain")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

void BenchmarkDrogonStyleWritePathPlain256(benchmark::State& state) {
  RunDrogonStyleWritePathPlain(state, 256);
}

void BenchmarkDrogonStyleWritePathPlain1024(benchmark::State& state) {
  RunDrogonStyleWritePathPlain(state, 1024);
}

void BenchmarkDrogonStyleWritePathPlain4096(benchmark::State& state) {
  RunDrogonStyleWritePathPlain(state, 4096);
}

BENCHMARK(BenchmarkDrogonStyleWritePathPlain256)
    ->Name("drogon_style_write_path_plain_256")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkDrogonStyleWritePathPlain1024)
    ->Name("drogon_style_write_path_plain_1024")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkDrogonStyleWritePathPlain4096)
    ->Name("drogon_style_write_path_plain_4096")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

void RunThirdPartyWebSocketStyleWritePathPlain(benchmark::State& state,
                                               size_t payload_size) {
  RawSocketPair pair;
  if (!pair.valid()) {
    state.SkipWithError("socketpair create failed");
    return;
  }

  const auto payload = BuildWritePayload(payload_size);
  const auto payload_bytes = std::span<const std::byte>(payload.data(),
                                                       payload.size());
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::vector<std::byte> peer_drain(ClientFrameWireBytes(payload_size));
  std::uint64_t frames_sent = 0;
  const size_t wire_bytes = ClientFrameWireBytes(payload_size);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    if (!ThirdPartyStyleSendBinary(pair.client_fd(), payload_bytes)) {
      state.SkipWithError("third-party-style write failed");
      return;
    }
    ++frames_sent;
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);

    state.PauseTiming();
    if (!ReadExactFd(pair.peer_fd(),
                     std::span<std::byte>(peer_drain.data(), wire_bytes))) {
      state.SkipWithError("peer drain failed");
      return;
    }
    state.ResumeTiming();
  }

  SetLatencyCounters(state, std::move(samples_ns), "frames_sent", frames_sent);
  state.SetLabel(BuildBenchmarkLabel(false,
                                     "third-party-websocket-local-socketpair",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkThirdPartyWebSocketStyleWritePathPlain(benchmark::State& state) {
  RunThirdPartyWebSocketStyleWritePathPlain(state, 64);
}

BENCHMARK(BenchmarkThirdPartyWebSocketStyleWritePathPlain)
    ->Name("third_party_websocket_style_write_path_plain")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

void BenchmarkThirdPartyWebSocketStyleWritePathPlain256(
    benchmark::State& state) {
  RunThirdPartyWebSocketStyleWritePathPlain(state, 256);
}

void BenchmarkThirdPartyWebSocketStyleWritePathPlain1024(
    benchmark::State& state) {
  RunThirdPartyWebSocketStyleWritePathPlain(state, 1024);
}

void BenchmarkThirdPartyWebSocketStyleWritePathPlain4096(
    benchmark::State& state) {
  RunThirdPartyWebSocketStyleWritePathPlain(state, 4096);
}

BENCHMARK(BenchmarkThirdPartyWebSocketStyleWritePathPlain256)
    ->Name("third_party_websocket_style_write_path_plain_256")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkThirdPartyWebSocketStyleWritePathPlain1024)
    ->Name("third_party_websocket_style_write_path_plain_1024")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkThirdPartyWebSocketStyleWritePathPlain4096)
    ->Name("third_party_websocket_style_write_path_plain_4096")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

void FillBusinessQueue(CriticalSession<OneWritePerDriveSocket>& session,
                       std::span<const std::byte> payload,
                       size_t slots) {
  for (size_t i = 0; i < slots; ++i) {
    PreparedWrite* write = session.TryAcquirePreparedWrite();
    if (write == nullptr) {
      return;
    }
    std::copy(payload.begin(), payload.end(), write->storage.begin());
    write->encoded_size = static_cast<std::uint32_t>(payload.size());
    write->kind = PayloadKind::kBinary;
    if (session.CommitPreparedWrite(write) != SendStatus::kOk) {
      session.CancelPreparedWrite(write);
      return;
    }
  }
}

void DrainWrites(CriticalSession<OneWritePerDriveSocket>& session,
                 OneWritePerDriveSocket& socket) {
  while (session.WantsWrite() && !session.ShouldReconnect()) {
    socket.ResetWriteBudget();
    session.DriveWrite();
  }
}

void BenchmarkSessionControlSlotFullBusinessQueue(benchmark::State& state) {
  ConnectionConfig config{};
  config.enable_tls = false;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 128;
  config.read_buffer_bytes = 4096;
  config.frame_buffer_bytes = 4096;
  config.heartbeat_interval_ms = 1;
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();

  OneWritePerDriveSocket socket;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  CriticalSession<OneWritePerDriveSocket> session(config, socket, arena,
                                                  metrics);
  const auto payload = BuildWritePayload();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t now_ns = 1'000'000ULL;

  for (auto _ : state) {
    state.PauseTiming();
    DrainWrites(session, socket);
    FillBusinessQueue(session, payload, config.prepared_write_slots);
    socket.ResetWriteBudget();
    now_ns += 2'000'000ULL;
    state.ResumeTiming();

    const std::uint64_t start_ns = NowNs();
    session.AdvanceHeartbeat(now_ns);
    session.DriveWrite();
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    if (session.ShouldReconnect()) {
      state.SkipWithError("session requested reconnect");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
  }

  SetLatencyCounters(state, std::move(samples_ns), "tx_messages",
                     metrics.tx_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "one-write-per-drive",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkSessionControlSlotFullBusinessQueue)
    ->Name("session_write_path_control_slot_full_business_queue")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
