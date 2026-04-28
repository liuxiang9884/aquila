#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/frame_codec.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/types.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

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

void BenchmarkSessionWritePathWithEncode(benchmark::State& state) {
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
  FrameCodec encoder(config.max_frame_payload_bytes, config.frame_buffer_bytes);
  const auto payload = BuildWritePayload();
  const auto payload_bytes = std::span<const std::byte>(payload.data(),
                                                       payload.size());
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::array<std::byte, 128> peer_drain{};

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

BENCHMARK(BenchmarkSessionWritePathWithEncode)
    ->Name("session_write_path_with_encode_plain")
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
