#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/frame_codec.h"
#include "third_party/websocket/websocket.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>
#include <trantor/utils/MsgBuffer.h>

namespace ws = aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

constexpr size_t kBatchSize = 64;
constexpr size_t kCompactBatchSize = 8;
constexpr size_t kLargeBatchSize = 8;
constexpr size_t kIterations = 4096;
constexpr size_t kCompactIterations = 1024;
constexpr size_t kLargeIterations = 1024;
constexpr size_t kCoalescedFrameCount = 16;
constexpr size_t kBurstFrameCount = 128;
constexpr size_t kThirdPartyRecvBufferBytes = 4096;
constexpr size_t kCompactReceiveBufferBytes = 64U * 1024U;
constexpr size_t kCompactFillerFrameBytes = 44U * 1024U;
constexpr size_t kCompactFillerPayloadBytes = kCompactFillerFrameBytes - 4U;
constexpr size_t kCompactPartialPayloadBytes = 24U * 1024U;
constexpr size_t kCompactPartialPrefixBytes = 16U * 1024U;
constexpr size_t kCompactMaxPayloadBytes =
    kCompactFillerPayloadBytes > kCompactPartialPayloadBytes
        ? kCompactFillerPayloadBytes
        : kCompactPartialPayloadBytes;
constexpr size_t kLargeReceiveBufferBytes = 64U * 1024U;
constexpr size_t kLargeFillerFrameBytes = 28U * 1024U;
constexpr size_t kLargeFillerPayloadBytes = kLargeFillerFrameBytes - 4U;
constexpr size_t kLargePayloadBytes = 48U * 1024U;
constexpr size_t kLargePartialPrefixBytes = 32U * 1024U;

std::string MakePayload(size_t size, char value) {
  return std::string(size, value);
}

std::vector<std::byte> BuildServerTextFrame(std::string_view payload) {
  const size_t header_bytes = payload.size() <= 125 ? 2 : 4;
  std::vector<std::byte> frame(header_bytes + payload.size());
  frame[0] = std::byte{0x81};
  if (payload.size() <= 125) {
    frame[1] = std::byte{static_cast<unsigned char>(payload.size())};
  } else {
    frame[1] = std::byte{126};
    frame[2] = std::byte{
        static_cast<unsigned char>((payload.size() >> 8U) & 0xFFU)};
    frame[3] = std::byte{static_cast<unsigned char>(payload.size() & 0xFFU)};
  }
  for (size_t i = 0; i < payload.size(); ++i) {
    frame[header_bytes + i] = static_cast<std::byte>(payload[i]);
  }
  return frame;
}

std::vector<std::byte> BuildCoalescedServerTextFrames(std::string_view payload,
                                                      size_t frame_count) {
  const auto frame = BuildServerTextFrame(payload);
  std::vector<std::byte> coalesced;
  coalesced.reserve(frame.size() * frame_count);
  for (size_t i = 0; i < frame_count; ++i) {
    coalesced.insert(coalesced.end(), frame.begin(), frame.end());
  }
  return coalesced;
}

struct SplitFrameBytes {
  std::vector<std::byte> setup;
  std::vector<std::byte> suffix;
};

SplitFrameBytes BuildSplitFrameBytes(size_t filler_payload_bytes,
                                     size_t target_payload_bytes,
                                     size_t target_prefix_bytes) {
  const auto filler =
      BuildServerTextFrame(MakePayload(filler_payload_bytes, 'f'));
  const auto target =
      BuildServerTextFrame(MakePayload(target_payload_bytes, 'p'));

  SplitFrameBytes bytes;
  bytes.setup.reserve(filler.size() + target_prefix_bytes);
  bytes.setup.insert(bytes.setup.end(), filler.begin(), filler.end());
  bytes.setup.insert(bytes.setup.end(), target.begin(),
                     target.begin() + target_prefix_bytes);
  bytes.suffix.insert(bytes.suffix.end(), target.begin() + target_prefix_bytes,
                      target.end());
  return bytes;
}

struct ThirdPartyHandler;

using ThirdPartyConnectionBase =
    websocket::WSConnection<ThirdPartyHandler, char, false,
                            kThirdPartyRecvBufferBytes, false>;

struct ThirdPartyHandler {
  std::uint64_t payload_bytes{0};
  std::uint64_t opcode_accumulator{0};

  void onWSMsg(ThirdPartyConnectionBase&, std::uint8_t opcode,
               const std::uint8_t* payload, std::uint32_t payload_size) {
    payload_bytes += payload_size;
    opcode_accumulator += opcode;
    benchmark::DoNotOptimize(payload);
  }
};

class ThirdPartyParseConnection final : public ThirdPartyConnectionBase {
 public:
  ThirdPartyParseConnection() { this->init(std::numeric_limits<std::uint64_t>::max()); }

  std::uint32_t Parse(ThirdPartyHandler& handler,
                      std::span<std::byte> frame) {
    return this->handleWSMsg(
        &handler, reinterpret_cast<std::uint8_t*>(frame.data()),
        static_cast<std::uint32_t>(frame.size()));
  }
};

class LinearFrameCodec {
 public:
  LinearFrameCodec(size_t max_payload_bytes, size_t receive_buffer_bytes)
      : max_payload_bytes_(max_payload_bytes), storage_(receive_buffer_bytes) {}

  void Reset() {
    head_ = 0;
    tail_ = 0;
    next_sequence_ = 0;
    compact_count_ = 0;
    compacted_bytes_ = 0;
    protocol_error_pending_ = false;
    capacity_error_pending_ = false;
    delivered_pending_ = false;
    delivered_frame_end_ = 0;
  }

  size_t compact_count() const { return compact_count_; }

  size_t compacted_bytes() const { return compacted_bytes_; }

  std::span<std::byte> WritableSpan() {
    ReleaseDeliveredFrame();
    if (!EnsureWritable(1)) {
      capacity_error_pending_ = true;
      return {};
    }
    return std::span<std::byte>(storage_.data() + tail_,
                                storage_.size() - tail_);
  }

  void CommitWritten(size_t bytes) {
    if (bytes > storage_.size() - tail_) {
      capacity_error_pending_ = true;
      return;
    }
    tail_ += bytes;
  }

  ws::DecodeResult Feed(std::span<const std::byte> bytes) {
    ReleaseDeliveredFrame();
    if (!EnsureWritable(bytes.size())) {
      capacity_error_pending_ = true;
    } else if (!bytes.empty()) {
      std::memcpy(storage_.data() + tail_, bytes.data(), bytes.size());
      tail_ += bytes.size();
    }
    return PollWithoutRelease();
  }

  ws::DecodeResult Poll() {
    ReleaseDeliveredFrame();
    return PollWithoutRelease();
  }

 private:
  ws::DecodeResult PollWithoutRelease() {
    if (protocol_error_pending_) {
      protocol_error_pending_ = false;
      return {ws::DecodeStatus::kProtocolError, {}};
    }
    if (capacity_error_pending_) {
      capacity_error_pending_ = false;
      return {ws::DecodeStatus::kCapacityExceeded, {}};
    }
    return ParseOneFrame();
  }

  ws::DecodeResult ParseOneFrame() {
    const size_t available = tail_ - head_;
    if (available < 2) {
      return {};
    }

    const auto* data = storage_.data() + head_;
    const std::uint8_t first = std::to_integer<std::uint8_t>(data[0]);
    const std::uint8_t second = std::to_integer<std::uint8_t>(data[1]);

    const bool fin = (first & 0x80U) != 0;
    if (!fin || (first & 0x70U) != 0) {
      return {ws::DecodeStatus::kProtocolError, {}};
    }

    ws::PayloadKind payload_kind{};
    if (!MapOpcode(first & 0x0FU, &payload_kind)) {
      return {ws::DecodeStatus::kProtocolError, {}};
    }
    if ((second & 0x80U) != 0) {
      return {ws::DecodeStatus::kProtocolError, {}};
    }

    size_t cursor = 2;
    std::uint64_t payload_length = second & 0x7FU;
    if (payload_length == 126) {
      if (available < cursor + 2) {
        return {};
      }
      payload_length =
          (static_cast<std::uint64_t>(
               std::to_integer<std::uint8_t>(data[cursor]))
           << 8) |
          static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(data[cursor + 1]));
      cursor += 2;
    } else if (payload_length == 127) {
      if (available < cursor + 8) {
        return {};
      }
      payload_length = 0;
      for (size_t i = 0; i < 8; ++i) {
        payload_length =
            (payload_length << 8) |
            static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(data[cursor + i]));
      }
      cursor += 8;
      if ((payload_length >> 63U) != 0) {
        return {ws::DecodeStatus::kProtocolError, {}};
      }
    }

    if (payload_length > max_payload_bytes_ ||
        payload_length >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max())) {
      return {ws::DecodeStatus::kProtocolError, {}};
    }
    if (IsControl(payload_kind) && payload_length > 125U) {
      return {ws::DecodeStatus::kProtocolError, {}};
    }

    const std::uint64_t total_frame_bytes = cursor + payload_length;
    if (available < total_frame_bytes) {
      return {};
    }
    if (total_frame_bytes > storage_.size()) {
      return {ws::DecodeStatus::kCapacityExceeded, {}};
    }

    const size_t frame_end = head_ + static_cast<size_t>(total_frame_bytes);
    delivered_pending_ = true;
    delivered_frame_end_ = frame_end;

    ws::MessageView view{};
    view.kind = payload_kind;
    view.payload = std::span<const std::byte>(
        storage_.data() + head_ + cursor, static_cast<size_t>(payload_length));
    view.sequence = next_sequence_++;
    view.fin = true;
    return {ws::DecodeStatus::kMessageReady, view};
  }

  void ReleaseDeliveredFrame() {
    if (!delivered_pending_) {
      return;
    }
    head_ = delivered_frame_end_;
    delivered_pending_ = false;
    if (head_ == tail_) {
      head_ = 0;
      tail_ = 0;
    }
  }

  bool EnsureWritable(size_t bytes) {
    if (bytes > storage_.size()) {
      return false;
    }
    if (storage_.size() - tail_ >= bytes) {
      return true;
    }
    Compact();
    return storage_.size() - tail_ >= bytes;
  }

  void Compact() {
    if (head_ == 0) {
      return;
    }
    const size_t remaining = tail_ - head_;
    ++compact_count_;
    compacted_bytes_ += remaining;
    if (remaining != 0) {
      std::memmove(storage_.data(), storage_.data() + head_, remaining);
    }
    tail_ = remaining;
    head_ = 0;
  }

  static bool MapOpcode(std::uint8_t opcode, ws::PayloadKind* kind) {
    switch (opcode) {
      case 0x1U:
        *kind = ws::PayloadKind::kText;
        return true;
      case 0x2U:
        *kind = ws::PayloadKind::kBinary;
        return true;
      case 0x8U:
        *kind = ws::PayloadKind::kClose;
        return true;
      case 0x9U:
        *kind = ws::PayloadKind::kPing;
        return true;
      case 0xAU:
        *kind = ws::PayloadKind::kPong;
        return true;
      default:
        return false;
    }
  }

  static bool IsControl(ws::PayloadKind kind) {
    return kind == ws::PayloadKind::kPing || kind == ws::PayloadKind::kPong ||
           kind == ws::PayloadKind::kClose;
  }

  size_t max_payload_bytes_{0};
  std::vector<std::byte> storage_{};
  size_t head_{0};
  size_t tail_{0};
  std::uint64_t next_sequence_{0};
  size_t compact_count_{0};
  size_t compacted_bytes_{0};
  bool protocol_error_pending_{false};
  bool capacity_error_pending_{false};
  bool delivered_pending_{false};
  size_t delivered_frame_end_{0};
};

struct DrogonDecodeResult {
  ws::DecodeStatus status{ws::DecodeStatus::kNeedMore};
  std::string message{};
  ws::PayloadKind kind{ws::PayloadKind::kText};
};

class DrogonFrameCodec {
 public:
  explicit DrogonFrameCodec(size_t receive_buffer_bytes)
      : buffer_(receive_buffer_bytes) {}

  void Reset() {
    buffer_.retrieveAll();
    message_.clear();
    type_ = ws::PayloadKind::kText;
    got_all_ = false;
  }

  void Append(std::span<const std::byte> bytes) {
    buffer_.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  DrogonDecodeResult Feed(std::span<const std::byte> bytes) {
    Append(bytes);
    return Poll();
  }

  DrogonDecodeResult Poll() {
    if (!Parse()) {
      return {ws::DecodeStatus::kProtocolError, {}, type_};
    }
    if (!got_all_) {
      return {};
    }
    std::string message;
    message.swap(message_);
    return {ws::DecodeStatus::kMessageReady, std::move(message), type_};
  }

 private:
  bool Parse() {
    got_all_ = false;
    while (buffer_.readableBytes() >= 2) {
      const auto first = static_cast<unsigned char>(buffer_[0]);
      const auto second = static_cast<unsigned char>(buffer_[1]);
      const unsigned char opcode = first & 0x0FU;
      bool is_control_frame = false;
      switch (opcode) {
        case 0:
          break;
        case 1:
          type_ = ws::PayloadKind::kText;
          break;
        case 2:
          type_ = ws::PayloadKind::kBinary;
          break;
        case 8:
          type_ = ws::PayloadKind::kClose;
          is_control_frame = true;
          break;
        case 9:
          type_ = ws::PayloadKind::kPing;
          is_control_frame = true;
          break;
        case 10:
          type_ = ws::PayloadKind::kPong;
          is_control_frame = true;
          break;
        default:
          return false;
      }

      const bool is_fin = (first & 0x80U) == 0x80U;
      if (!is_fin && is_control_frame) {
        return false;
      }

      size_t length = second & 0x7FU;
      const bool is_masked = (second & 0x80U) != 0;
      size_t index_first_mask = 2;
      if (length == 126) {
        index_first_mask = 4;
      } else if (length == 127) {
        index_first_mask = 10;
      }

      if (index_first_mask > 2) {
        if (buffer_.readableBytes() < index_first_mask) {
          return true;
        }
        if (is_control_frame) {
          return false;
        }
        if (index_first_mask == 4) {
          length = static_cast<unsigned char>(buffer_[2]);
          length = (length << 8U) + static_cast<unsigned char>(buffer_[3]);
        } else {
          length = 0;
          for (size_t i = 2; i <= 9; ++i) {
            if (length > (std::numeric_limits<size_t>::max() >> 8U)) {
              return false;
            }
            length =
                (length << 8U) + static_cast<unsigned char>(buffer_[i]);
          }
        }
      }

      if (is_masked) {
        const size_t index_first_data_byte = index_first_mask + 4U;
        if (buffer_.readableBytes() < index_first_data_byte + length) {
          return true;
        }
        const char* masks = buffer_.peek() + index_first_mask;
        const char* raw_data = buffer_.peek() + index_first_data_byte;
        const size_t old_length = message_.length();
        message_.resize(old_length + length);
        for (size_t i = 0; i < length; ++i) {
          message_[old_length + i] = raw_data[i] ^ masks[i & 0x3U];
        }
        buffer_.retrieve(index_first_data_byte + length);
      } else {
        if (buffer_.readableBytes() < index_first_mask + length) {
          return true;
        }
        const char* raw_data = buffer_.peek() + index_first_mask;
        message_.append(raw_data, length);
        buffer_.retrieve(index_first_mask + length);
      }

      if (is_fin) {
        got_all_ = true;
        return true;
      }
    }
    return true;
  }

  trantor::MsgBuffer buffer_;
  std::string message_;
  ws::PayloadKind type_{ws::PayloadKind::kText};
  bool got_all_{false};
};

void SetCommonCounters(benchmark::State& state,
                       std::vector<std::uint64_t> samples_ns,
                       std::uint64_t payload_bytes,
                       std::uint64_t opcode_accumulator) {
  SetLatencyCounters(state, std::move(samples_ns), "payload_bytes",
                     payload_bytes + opcode_accumulator);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void SetCoalescedCounters(benchmark::State& state,
                          std::vector<std::uint64_t> samples_ns,
                          std::uint64_t payload_bytes,
                          std::uint64_t opcode_accumulator) {
  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
  state.counters["frames_per_read"] =
      static_cast<double>(kCoalescedFrameCount);
}

void SetFramesPerReadCounters(benchmark::State& state,
                              std::vector<std::uint64_t> samples_ns,
                              std::uint64_t payload_bytes,
                              std::uint64_t opcode_accumulator,
                              size_t frames_per_read) {
  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
  state.counters["frames_per_read"] = static_cast<double>(frames_per_read);
}

void SetDrogonCounters(benchmark::State& state,
                       std::vector<std::uint64_t> samples_ns,
                       std::uint64_t payload_bytes,
                       std::uint64_t opcode_accumulator) {
  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
  state.counters["payload_copy_bytes"] = static_cast<double>(payload_bytes);
}

void SetDrogonFramesPerReadCounters(benchmark::State& state,
                                    std::vector<std::uint64_t> samples_ns,
                                    std::uint64_t payload_bytes,
                                    std::uint64_t opcode_accumulator,
                                    size_t frames_per_read) {
  SetDrogonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
  state.counters["frames_per_read"] = static_cast<double>(frames_per_read);
}

void BenchmarkThirdPartyHandleWSMsg(benchmark::State& state) {
  ThirdPartyParseConnection connection;
  ThirdPartyHandler handler;
  auto frame = BuildServerTextFrame("tick");
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const std::uint32_t remaining = connection.Parse(handler, frame);
      if (remaining != 0) {
        state.SkipWithError("third-party parser left unread bytes");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCommonCounters(state, std::move(samples_ns), handler.payload_bytes,
                    handler.opcode_accumulator);
}

void BenchmarkThirdPartyCoalescedDrain(benchmark::State& state) {
  ThirdPartyParseConnection connection;
  ThirdPartyHandler handler;
  auto coalesced = BuildCoalescedServerTextFrames("tick", kCoalescedFrameCount);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      std::span<std::byte> unread(coalesced.data(), coalesced.size());
      size_t decoded_frames = 0;
      while (!unread.empty()) {
        const std::uint32_t remaining = connection.Parse(handler, unread);
        if (remaining >= unread.size()) {
          state.SkipWithError("third-party coalesced parser made no progress");
          return;
        }
        const size_t consumed = unread.size() - remaining;
        unread = std::span<std::byte>(unread.data() + consumed, remaining);
        ++decoded_frames;
      }
      if (decoded_frames != kCoalescedFrameCount) {
        state.SkipWithError("third-party coalesced frame count mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kCoalescedFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCoalescedCounters(state, std::move(samples_ns), handler.payload_bytes,
                       handler.opcode_accumulator);
}

void BenchmarkAquilaFeedDecode(benchmark::State& state) {
  ws::FrameCodec codec(1024, 4096, 1024);
  const auto frame = BuildServerTextFrame("tick");
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto decoded = codec.Feed(frame);
      if (decoded.status != ws::DecodeStatus::kMessageReady) {
        state.SkipWithError("aquila Feed decode failed");
        return;
      }
      payload_bytes += decoded.view.payload.size();
      opcode_accumulator +=
          decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.view.payload.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
}

void BenchmarkAquilaCoalescedFeedDrain(benchmark::State& state) {
  ws::FrameCodec codec(1024, 4096, 1024);
  const auto coalesced =
      BuildCoalescedServerTextFrames("tick", kCoalescedFrameCount);
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      auto decoded = codec.Feed(coalesced);
      size_t decoded_frames = 0;
      while (decoded.status == ws::DecodeStatus::kMessageReady) {
        payload_bytes += decoded.view.payload.size();
        opcode_accumulator +=
            decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
        benchmark::DoNotOptimize(decoded.view.payload.data());
        ++decoded_frames;
        decoded = codec.Poll();
      }
      if (decoded.status != ws::DecodeStatus::kNeedMore) {
        state.SkipWithError("aquila coalesced Feed drain failed");
        return;
      }
      if (decoded_frames != kCoalescedFrameCount) {
        state.SkipWithError("aquila coalesced Feed frame count mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kCoalescedFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCoalescedCounters(state, std::move(samples_ns), payload_bytes,
                       opcode_accumulator);
}

void PrepareAquilaDirectFrame(ws::FrameCodec& codec,
                              std::span<const std::byte> frame) {
  codec.Reset();
  auto writable = codec.WritableSpan();
  std::copy(frame.begin(), frame.end(), writable.begin());
  codec.CommitWritten(frame.size());
}

void BenchmarkAquilaDirectPollDecode(benchmark::State& state) {
  const auto frame = BuildServerTextFrame("tick");
  std::vector<ws::FrameCodec> codecs;
  codecs.reserve(kBatchSize);
  for (size_t i = 0; i < kBatchSize; ++i) {
    codecs.emplace_back(1024, 4096, 1024);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      PrepareAquilaDirectFrame(codec, frame);
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      const auto decoded = codec.Poll();
      if (decoded.status != ws::DecodeStatus::kMessageReady) {
        state.SkipWithError("aquila direct Poll decode failed");
        return;
      }
      payload_bytes += decoded.view.payload.size();
      opcode_accumulator +=
          decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.view.payload.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
}

void BenchmarkAquilaCoalescedDirectPollDrain(benchmark::State& state) {
  const auto coalesced =
      BuildCoalescedServerTextFrames("tick", kCoalescedFrameCount);
  std::vector<ws::FrameCodec> codecs;
  codecs.reserve(kBatchSize);
  for (size_t i = 0; i < kBatchSize; ++i) {
    codecs.emplace_back(1024, 4096, 1024);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      PrepareAquilaDirectFrame(codec, coalesced);
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      size_t decoded_frames = 0;
      while (true) {
        const auto decoded = codec.Poll();
        if (decoded.status == ws::DecodeStatus::kNeedMore) {
          break;
        }
        if (decoded.status != ws::DecodeStatus::kMessageReady) {
          state.SkipWithError("aquila coalesced direct Poll drain failed");
          return;
        }
        payload_bytes += decoded.view.payload.size();
        opcode_accumulator +=
            decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
        benchmark::DoNotOptimize(decoded.view.payload.data());
        ++decoded_frames;
      }
      if (decoded_frames != kCoalescedFrameCount) {
        state.SkipWithError("aquila coalesced direct frame count mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kCoalescedFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCoalescedCounters(state, std::move(samples_ns), payload_bytes,
                       opcode_accumulator);
}

void PrepareLinearDirectFrame(LinearFrameCodec& codec,
                              std::span<const std::byte> frame) {
  codec.Reset();
  auto writable = codec.WritableSpan();
  std::copy(frame.begin(), frame.end(), writable.begin());
  codec.CommitWritten(frame.size());
}

void BenchmarkAquilaLinearFeedDecode(benchmark::State& state) {
  LinearFrameCodec codec(1024, 4096);
  const auto frame = BuildServerTextFrame("tick");
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto decoded = codec.Feed(frame);
      if (decoded.status != ws::DecodeStatus::kMessageReady) {
        state.SkipWithError("aquila linear Feed decode failed");
        return;
      }
      payload_bytes += decoded.view.payload.size();
      opcode_accumulator +=
          decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.view.payload.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
}

void BenchmarkAquilaLinearDirectPollDecode(benchmark::State& state) {
  const auto frame = BuildServerTextFrame("tick");
  std::vector<LinearFrameCodec> codecs;
  codecs.reserve(kBatchSize);
  for (size_t i = 0; i < kBatchSize; ++i) {
    codecs.emplace_back(1024, 4096);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      PrepareLinearDirectFrame(codec, frame);
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      const auto decoded = codec.Poll();
      if (decoded.status != ws::DecodeStatus::kMessageReady) {
        state.SkipWithError("aquila linear direct Poll decode failed");
        return;
      }
      payload_bytes += decoded.view.payload.size();
      opcode_accumulator +=
          decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.view.payload.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
}

void BenchmarkAquilaLinearCoalescedFeedDrain(benchmark::State& state) {
  LinearFrameCodec codec(1024, 4096);
  const auto coalesced =
      BuildCoalescedServerTextFrames("tick", kCoalescedFrameCount);
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      auto decoded = codec.Feed(coalesced);
      size_t decoded_frames = 0;
      while (decoded.status == ws::DecodeStatus::kMessageReady) {
        payload_bytes += decoded.view.payload.size();
        opcode_accumulator +=
            decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
        benchmark::DoNotOptimize(decoded.view.payload.data());
        ++decoded_frames;
        decoded = codec.Poll();
      }
      if (decoded.status != ws::DecodeStatus::kNeedMore) {
        state.SkipWithError("aquila linear coalesced Feed drain failed");
        return;
      }
      if (decoded_frames != kCoalescedFrameCount) {
        state.SkipWithError("aquila linear coalesced Feed frame mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kCoalescedFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCoalescedCounters(state, std::move(samples_ns), payload_bytes,
                       opcode_accumulator);
}

void BenchmarkAquilaLinearCoalescedDirectPollDrain(benchmark::State& state) {
  const auto coalesced =
      BuildCoalescedServerTextFrames("tick", kCoalescedFrameCount);
  std::vector<LinearFrameCodec> codecs;
  codecs.reserve(kBatchSize);
  for (size_t i = 0; i < kBatchSize; ++i) {
    codecs.emplace_back(1024, 4096);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      PrepareLinearDirectFrame(codec, coalesced);
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      size_t decoded_frames = 0;
      while (true) {
        const auto decoded = codec.Poll();
        if (decoded.status == ws::DecodeStatus::kNeedMore) {
          break;
        }
        if (decoded.status != ws::DecodeStatus::kMessageReady) {
          state.SkipWithError("aquila linear coalesced direct Poll failed");
          return;
        }
        payload_bytes += decoded.view.payload.size();
        opcode_accumulator +=
            decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
        benchmark::DoNotOptimize(decoded.view.payload.data());
        ++decoded_frames;
      }
      if (decoded_frames != kCoalescedFrameCount) {
        state.SkipWithError("aquila linear coalesced direct frame mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kCoalescedFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCoalescedCounters(state, std::move(samples_ns), payload_bytes,
                       opcode_accumulator);
}

void PrepareDrogonDirectFrame(DrogonFrameCodec& codec,
                              std::span<const std::byte> frame) {
  codec.Reset();
  codec.Append(frame);
}

void BenchmarkDrogonFeedDecode(benchmark::State& state) {
  DrogonFrameCodec codec(4096);
  const auto frame = BuildServerTextFrame("tick");
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto decoded = codec.Feed(frame);
      if (decoded.status != ws::DecodeStatus::kMessageReady) {
        state.SkipWithError("drogon Feed decode failed");
        return;
      }
      payload_bytes += decoded.message.size();
      opcode_accumulator +=
          decoded.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.message.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetDrogonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
}

void BenchmarkDrogonDirectPollDecode(benchmark::State& state) {
  const auto frame = BuildServerTextFrame("tick");
  std::vector<DrogonFrameCodec> codecs;
  codecs.reserve(kBatchSize);
  for (size_t i = 0; i < kBatchSize; ++i) {
    codecs.emplace_back(4096);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      PrepareDrogonDirectFrame(codec, frame);
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      const auto decoded = codec.Poll();
      if (decoded.status != ws::DecodeStatus::kMessageReady) {
        state.SkipWithError("drogon direct Poll decode failed");
        return;
      }
      payload_bytes += decoded.message.size();
      opcode_accumulator +=
          decoded.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.message.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetDrogonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
}

void BenchmarkDrogonCoalescedFeedDrain(benchmark::State& state) {
  DrogonFrameCodec codec(4096);
  const auto coalesced =
      BuildCoalescedServerTextFrames("tick", kCoalescedFrameCount);
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      auto decoded = codec.Feed(coalesced);
      size_t decoded_frames = 0;
      while (decoded.status == ws::DecodeStatus::kMessageReady) {
        payload_bytes += decoded.message.size();
        opcode_accumulator +=
            decoded.kind == ws::PayloadKind::kText ? 1U : 0U;
        benchmark::DoNotOptimize(decoded.message.data());
        ++decoded_frames;
        decoded = codec.Poll();
      }
      if (decoded.status != ws::DecodeStatus::kNeedMore) {
        state.SkipWithError("drogon coalesced Feed drain failed");
        return;
      }
      if (decoded_frames != kCoalescedFrameCount) {
        state.SkipWithError("drogon coalesced Feed frame count mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kCoalescedFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetDrogonFramesPerReadCounters(state, std::move(samples_ns), payload_bytes,
                                 opcode_accumulator, kCoalescedFrameCount);
}

void BenchmarkDrogonCoalescedDirectPollDrain(benchmark::State& state) {
  const auto coalesced =
      BuildCoalescedServerTextFrames("tick", kCoalescedFrameCount);
  std::vector<DrogonFrameCodec> codecs;
  codecs.reserve(kBatchSize);
  for (size_t i = 0; i < kBatchSize; ++i) {
    codecs.emplace_back(4096);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      PrepareDrogonDirectFrame(codec, coalesced);
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      size_t decoded_frames = 0;
      while (true) {
        const auto decoded = codec.Poll();
        if (decoded.status == ws::DecodeStatus::kNeedMore) {
          break;
        }
        if (decoded.status != ws::DecodeStatus::kMessageReady) {
          state.SkipWithError("drogon coalesced direct Poll failed");
          return;
        }
        payload_bytes += decoded.message.size();
        opcode_accumulator +=
            decoded.kind == ws::PayloadKind::kText ? 1U : 0U;
        benchmark::DoNotOptimize(decoded.message.data());
        ++decoded_frames;
      }
      if (decoded_frames != kCoalescedFrameCount) {
        state.SkipWithError("drogon coalesced direct frame mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kCoalescedFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetDrogonFramesPerReadCounters(state, std::move(samples_ns), payload_bytes,
                                 opcode_accumulator, kCoalescedFrameCount);
}

void BenchmarkAquilaCompactPressureFeedDecode(benchmark::State& state) {
  const auto split =
      BuildSplitFrameBytes(kCompactFillerPayloadBytes,
                           kCompactPartialPayloadBytes,
                           kCompactPartialPrefixBytes);
  if (split.setup.size() > kCompactReceiveBufferBytes ||
      split.suffix.size() <= kCompactReceiveBufferBytes - split.setup.size()) {
    state.SkipWithError("compact-pressure setup does not force linear compact");
    return;
  }

  std::vector<ws::FrameCodec> codecs;
  codecs.reserve(kCompactBatchSize);
  for (size_t i = 0; i < kCompactBatchSize; ++i) {
    codecs.emplace_back(kCompactMaxPayloadBytes, kCompactReceiveBufferBytes,
                        1024);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kCompactIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      codec.Reset();
      const auto prepared = codec.Feed(split.setup);
      if (prepared.status != ws::DecodeStatus::kMessageReady ||
          prepared.view.payload.size() != kCompactFillerPayloadBytes) {
        state.SkipWithError("aquila compact-pressure setup failed");
        return;
      }
      benchmark::DoNotOptimize(prepared.view.payload.data());
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      const auto decoded = codec.Feed(split.suffix);
      if (decoded.status != ws::DecodeStatus::kMessageReady ||
          decoded.view.payload.size() != kCompactPartialPayloadBytes) {
        state.SkipWithError("aquila compact-pressure decode failed");
        return;
      }
      payload_bytes += decoded.view.payload.size();
      opcode_accumulator +=
          decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.view.payload.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kCompactBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
  state.counters["setup_bytes"] = static_cast<double>(split.setup.size());
  state.counters["suffix_bytes"] = static_cast<double>(split.suffix.size());
  state.counters["compacted_bytes"] = 0.0;
  state.counters["compact_events"] = 0.0;
}

void BenchmarkAquilaLinearCompactPressureFeedDecode(
    benchmark::State& state) {
  const auto split =
      BuildSplitFrameBytes(kCompactFillerPayloadBytes,
                           kCompactPartialPayloadBytes,
                           kCompactPartialPrefixBytes);
  if (split.setup.size() > kCompactReceiveBufferBytes ||
      split.suffix.size() <= kCompactReceiveBufferBytes - split.setup.size()) {
    state.SkipWithError("linear compact-pressure setup does not force compact");
    return;
  }

  std::vector<LinearFrameCodec> codecs;
  codecs.reserve(kCompactBatchSize);
  for (size_t i = 0; i < kCompactBatchSize; ++i) {
    codecs.emplace_back(kCompactMaxPayloadBytes, kCompactReceiveBufferBytes);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::uint64_t compact_events = 0;
  std::uint64_t compacted_bytes = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kCompactIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      codec.Reset();
      const auto prepared = codec.Feed(split.setup);
      if (prepared.status != ws::DecodeStatus::kMessageReady ||
          prepared.view.payload.size() != kCompactFillerPayloadBytes) {
        state.SkipWithError("aquila linear compact-pressure setup failed");
        return;
      }
      benchmark::DoNotOptimize(prepared.view.payload.data());
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      const auto decoded = codec.Feed(split.suffix);
      if (decoded.status != ws::DecodeStatus::kMessageReady ||
          decoded.view.payload.size() != kCompactPartialPayloadBytes) {
        state.SkipWithError("aquila linear compact-pressure decode failed");
        return;
      }
      payload_bytes += decoded.view.payload.size();
      opcode_accumulator +=
          decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
      compact_events += codec.compact_count();
      compacted_bytes += codec.compacted_bytes();
      benchmark::DoNotOptimize(decoded.view.payload.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kCompactBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
  state.counters["setup_bytes"] = static_cast<double>(split.setup.size());
  state.counters["suffix_bytes"] = static_cast<double>(split.suffix.size());
  state.counters["compacted_bytes"] = static_cast<double>(compacted_bytes);
  state.counters["compact_events"] = static_cast<double>(compact_events);
}

void BenchmarkDrogonCompactPressureFeedDecode(benchmark::State& state) {
  const auto split =
      BuildSplitFrameBytes(kCompactFillerPayloadBytes,
                           kCompactPartialPayloadBytes,
                           kCompactPartialPrefixBytes);
  if (split.setup.size() > kCompactReceiveBufferBytes ||
      split.suffix.size() <= kCompactReceiveBufferBytes - split.setup.size()) {
    state.SkipWithError("drogon compact-pressure setup does not force compact");
    return;
  }

  std::vector<DrogonFrameCodec> codecs;
  codecs.reserve(kCompactBatchSize);
  for (size_t i = 0; i < kCompactBatchSize; ++i) {
    codecs.emplace_back(kCompactReceiveBufferBytes);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kCompactIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      codec.Reset();
      const auto prepared = codec.Feed(split.setup);
      if (prepared.status != ws::DecodeStatus::kMessageReady ||
          prepared.message.size() != kCompactFillerPayloadBytes) {
        state.SkipWithError("drogon compact-pressure setup failed");
        return;
      }
      benchmark::DoNotOptimize(prepared.message.data());
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      const auto decoded = codec.Feed(split.suffix);
      if (decoded.status != ws::DecodeStatus::kMessageReady ||
          decoded.message.size() != kCompactPartialPayloadBytes) {
        state.SkipWithError("drogon compact-pressure decode failed");
        return;
      }
      payload_bytes += decoded.message.size();
      opcode_accumulator +=
          decoded.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.message.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kCompactBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetDrogonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
  state.counters["setup_bytes"] = static_cast<double>(split.setup.size());
  state.counters["suffix_bytes"] = static_cast<double>(split.suffix.size());
}

void BenchmarkAquilaBurstFeedDrain(benchmark::State& state) {
  ws::FrameCodec codec(1024, 4096, 1024);
  const auto burst = BuildCoalescedServerTextFrames("tick", kBurstFrameCount);
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      auto decoded = codec.Feed(burst);
      size_t decoded_frames = 0;
      while (decoded.status == ws::DecodeStatus::kMessageReady) {
        payload_bytes += decoded.view.payload.size();
        opcode_accumulator +=
            decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
        benchmark::DoNotOptimize(decoded.view.payload.data());
        ++decoded_frames;
        decoded = codec.Poll();
      }
      if (decoded.status != ws::DecodeStatus::kNeedMore) {
        state.SkipWithError("aquila burst Feed drain failed");
        return;
      }
      if (decoded_frames != kBurstFrameCount) {
        state.SkipWithError("aquila burst Feed frame count mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kBurstFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetFramesPerReadCounters(state, std::move(samples_ns), payload_bytes,
                           opcode_accumulator, kBurstFrameCount);
}

void BenchmarkAquilaLinearBurstFeedDrain(benchmark::State& state) {
  LinearFrameCodec codec(1024, 4096);
  const auto burst = BuildCoalescedServerTextFrames("tick", kBurstFrameCount);
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      auto decoded = codec.Feed(burst);
      size_t decoded_frames = 0;
      while (decoded.status == ws::DecodeStatus::kMessageReady) {
        payload_bytes += decoded.view.payload.size();
        opcode_accumulator +=
            decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
        benchmark::DoNotOptimize(decoded.view.payload.data());
        ++decoded_frames;
        decoded = codec.Poll();
      }
      if (decoded.status != ws::DecodeStatus::kNeedMore) {
        state.SkipWithError("aquila linear burst Feed drain failed");
        return;
      }
      if (decoded_frames != kBurstFrameCount) {
        state.SkipWithError("aquila linear burst Feed frame count mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kBurstFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetFramesPerReadCounters(state, std::move(samples_ns), payload_bytes,
                           opcode_accumulator, kBurstFrameCount);
}

void BenchmarkDrogonBurstFeedDrain(benchmark::State& state) {
  DrogonFrameCodec codec(4096);
  const auto burst = BuildCoalescedServerTextFrames("tick", kBurstFrameCount);
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      auto decoded = codec.Feed(burst);
      size_t decoded_frames = 0;
      while (decoded.status == ws::DecodeStatus::kMessageReady) {
        payload_bytes += decoded.message.size();
        opcode_accumulator +=
            decoded.kind == ws::PayloadKind::kText ? 1U : 0U;
        benchmark::DoNotOptimize(decoded.message.data());
        ++decoded_frames;
        decoded = codec.Poll();
      }
      if (decoded.status != ws::DecodeStatus::kNeedMore) {
        state.SkipWithError("drogon burst Feed drain failed");
        return;
      }
      if (decoded_frames != kBurstFrameCount) {
        state.SkipWithError("drogon burst Feed frame count mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kBurstFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetDrogonFramesPerReadCounters(state, std::move(samples_ns), payload_bytes,
                                 opcode_accumulator, kBurstFrameCount);
}

void BenchmarkAquilaLargePayloadBoundaryFeedDecode(benchmark::State& state) {
  const auto split =
      BuildSplitFrameBytes(kLargeFillerPayloadBytes, kLargePayloadBytes,
                           kLargePartialPrefixBytes);
  if (split.setup.size() > kLargeReceiveBufferBytes ||
      split.suffix.size() <= kLargeReceiveBufferBytes - split.setup.size() ||
      split.suffix.size() > kLargeReceiveBufferBytes -
                                kLargePartialPrefixBytes) {
    state.SkipWithError("large-payload setup does not force linear compact");
    return;
  }

  std::vector<ws::FrameCodec> codecs;
  codecs.reserve(kLargeBatchSize);
  for (size_t i = 0; i < kLargeBatchSize; ++i) {
    codecs.emplace_back(kLargePayloadBytes, kLargeReceiveBufferBytes, 1024);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLargeIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      codec.Reset();
      const auto prepared = codec.Feed(split.setup);
      if (prepared.status != ws::DecodeStatus::kMessageReady ||
          prepared.view.payload.size() != kLargeFillerPayloadBytes) {
        state.SkipWithError("aquila large-payload setup failed");
        return;
      }
      benchmark::DoNotOptimize(prepared.view.payload.data());
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      const auto decoded = codec.Feed(split.suffix);
      if (decoded.status != ws::DecodeStatus::kMessageReady ||
          decoded.view.payload.size() != kLargePayloadBytes) {
        state.SkipWithError("aquila large-payload decode failed");
        return;
      }
      payload_bytes += decoded.view.payload.size();
      opcode_accumulator +=
          decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.view.payload.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kLargeBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
  state.counters["partial_prefix_bytes"] =
      static_cast<double>(kLargePartialPrefixBytes);
  state.counters["setup_bytes"] = static_cast<double>(split.setup.size());
  state.counters["suffix_bytes"] = static_cast<double>(split.suffix.size());
  state.counters["compacted_bytes"] = 0.0;
  state.counters["compact_events"] = 0.0;
}

void BenchmarkAquilaLinearLargePayloadBoundaryFeedDecode(
    benchmark::State& state) {
  const auto split =
      BuildSplitFrameBytes(kLargeFillerPayloadBytes, kLargePayloadBytes,
                           kLargePartialPrefixBytes);
  if (split.setup.size() > kLargeReceiveBufferBytes ||
      split.suffix.size() <= kLargeReceiveBufferBytes - split.setup.size() ||
      split.suffix.size() > kLargeReceiveBufferBytes -
                                kLargePartialPrefixBytes) {
    state.SkipWithError(
        "linear large-payload setup does not force compact");
    return;
  }

  std::vector<LinearFrameCodec> codecs;
  codecs.reserve(kLargeBatchSize);
  for (size_t i = 0; i < kLargeBatchSize; ++i) {
    codecs.emplace_back(kLargePayloadBytes, kLargeReceiveBufferBytes);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::uint64_t compact_events = 0;
  std::uint64_t compacted_bytes = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLargeIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      codec.Reset();
      const auto prepared = codec.Feed(split.setup);
      if (prepared.status != ws::DecodeStatus::kMessageReady ||
          prepared.view.payload.size() != kLargeFillerPayloadBytes) {
        state.SkipWithError("aquila linear large-payload setup failed");
        return;
      }
      benchmark::DoNotOptimize(prepared.view.payload.data());
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      const auto decoded = codec.Feed(split.suffix);
      if (decoded.status != ws::DecodeStatus::kMessageReady ||
          decoded.view.payload.size() != kLargePayloadBytes) {
        state.SkipWithError("aquila linear large-payload decode failed");
        return;
      }
      payload_bytes += decoded.view.payload.size();
      opcode_accumulator +=
          decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
      compact_events += codec.compact_count();
      compacted_bytes += codec.compacted_bytes();
      benchmark::DoNotOptimize(decoded.view.payload.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kLargeBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
  state.counters["partial_prefix_bytes"] =
      static_cast<double>(kLargePartialPrefixBytes);
  state.counters["setup_bytes"] = static_cast<double>(split.setup.size());
  state.counters["suffix_bytes"] = static_cast<double>(split.suffix.size());
  state.counters["compacted_bytes"] = static_cast<double>(compacted_bytes);
  state.counters["compact_events"] = static_cast<double>(compact_events);
}

void BenchmarkDrogonLargePayloadBoundaryFeedDecode(
    benchmark::State& state) {
  const auto split =
      BuildSplitFrameBytes(kLargeFillerPayloadBytes, kLargePayloadBytes,
                           kLargePartialPrefixBytes);
  if (split.setup.size() > kLargeReceiveBufferBytes ||
      split.suffix.size() <= kLargeReceiveBufferBytes - split.setup.size() ||
      split.suffix.size() > kLargeReceiveBufferBytes -
                                kLargePartialPrefixBytes) {
    state.SkipWithError(
        "drogon large-payload setup does not force compact");
    return;
  }

  std::vector<DrogonFrameCodec> codecs;
  codecs.reserve(kLargeBatchSize);
  for (size_t i = 0; i < kLargeBatchSize; ++i) {
    codecs.emplace_back(kLargeReceiveBufferBytes);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLargeIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      codec.Reset();
      const auto prepared = codec.Feed(split.setup);
      if (prepared.status != ws::DecodeStatus::kMessageReady ||
          prepared.message.size() != kLargeFillerPayloadBytes) {
        state.SkipWithError("drogon large-payload setup failed");
        return;
      }
      benchmark::DoNotOptimize(prepared.message.data());
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      const auto decoded = codec.Feed(split.suffix);
      if (decoded.status != ws::DecodeStatus::kMessageReady ||
          decoded.message.size() != kLargePayloadBytes) {
        state.SkipWithError("drogon large-payload decode failed");
        return;
      }
      payload_bytes += decoded.message.size();
      opcode_accumulator +=
          decoded.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.message.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kLargeBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetDrogonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
  state.counters["partial_prefix_bytes"] =
      static_cast<double>(kLargePartialPrefixBytes);
  state.counters["setup_bytes"] = static_cast<double>(split.setup.size());
  state.counters["suffix_bytes"] = static_cast<double>(split.suffix.size());
}

void AlignCodecNearMirroredBoundary(ws::FrameCodec& codec,
                                    std::span<const std::byte> filler) {
  codec.Reset();
  const size_t target_offset = codec.ReceiveCapacity() - 5U;
  const size_t filler_count = target_offset / filler.size();
  for (size_t i = 0; i < filler_count; ++i) {
    const auto decoded = codec.Feed(filler);
    benchmark::DoNotOptimize(decoded.view.payload.data());
    benchmark::DoNotOptimize(codec.Poll().status);
  }
}

void BenchmarkAquilaDirectPollMirroredBoundary(benchmark::State& state) {
  const auto filler = BuildServerTextFrame("x");
  const auto wrapped = BuildServerTextFrame("abcdef");
  std::vector<ws::FrameCodec> codecs;
  codecs.reserve(kBatchSize);
  for (size_t i = 0; i < kBatchSize; ++i) {
    codecs.emplace_back(128, 4096, 1024);
  }
  std::uint64_t payload_bytes = 0;
  std::uint64_t opcode_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kIterations);

  for (auto _ : state) {
    for (auto& codec : codecs) {
      AlignCodecNearMirroredBoundary(codec, filler);
      auto writable = codec.WritableSpan();
      if (writable.size() < wrapped.size()) {
        state.SkipWithError("insufficient aquila mirrored writable span");
        return;
      }
      std::copy(wrapped.begin(), wrapped.end(), writable.begin());
      codec.CommitWritten(wrapped.size());
    }

    const std::uint64_t start_ns = NowNs();
    for (auto& codec : codecs) {
      const auto decoded = codec.Poll();
      if (decoded.status != ws::DecodeStatus::kMessageReady) {
        state.SkipWithError("aquila mirrored boundary Poll decode failed");
        return;
      }
      payload_bytes += decoded.view.payload.size();
      opcode_accumulator +=
          decoded.view.kind == ws::PayloadKind::kText ? 1U : 0U;
      benchmark::DoNotOptimize(decoded.view.payload.data());
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetCommonCounters(state, std::move(samples_ns), payload_bytes,
                    opcode_accumulator);
}

BENCHMARK(BenchmarkThirdPartyHandleWSMsg)
    ->Name("third_party_handle_ws_msg")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaFeedDecode)
    ->Name("aquila_feed_decode")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaDirectPollDecode)
    ->Name("aquila_direct_poll_decode")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaDirectPollMirroredBoundary)
    ->Name("aquila_direct_poll_mirrored_boundary")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkThirdPartyCoalescedDrain)
    ->Name("third_party_coalesced_drain")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaCoalescedFeedDrain)
    ->Name("aquila_coalesced_feed_drain")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaCoalescedDirectPollDrain)
    ->Name("aquila_coalesced_direct_poll_drain")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaLinearFeedDecode)
    ->Name("aquila_linear_feed_decode")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaLinearDirectPollDecode)
    ->Name("aquila_linear_direct_poll_decode")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaLinearCoalescedFeedDrain)
    ->Name("aquila_linear_coalesced_feed_drain")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaLinearCoalescedDirectPollDrain)
    ->Name("aquila_linear_coalesced_direct_poll_drain")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkDrogonFeedDecode)
    ->Name("drogon_feed_decode")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkDrogonDirectPollDecode)
    ->Name("drogon_direct_poll_decode")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkDrogonCoalescedFeedDrain)
    ->Name("drogon_coalesced_feed_drain")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkDrogonCoalescedDirectPollDrain)
    ->Name("drogon_coalesced_direct_poll_drain")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaCompactPressureFeedDecode)
    ->Name("aquila_compact_pressure_feed_decode")
    ->Iterations(kCompactIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaLinearCompactPressureFeedDecode)
    ->Name("aquila_linear_compact_pressure_feed_decode")
    ->Iterations(kCompactIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkDrogonCompactPressureFeedDecode)
    ->Name("drogon_compact_pressure_feed_decode")
    ->Iterations(kCompactIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaBurstFeedDrain)
    ->Name("aquila_burst_feed_drain")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaLinearBurstFeedDrain)
    ->Name("aquila_linear_burst_feed_drain")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkDrogonBurstFeedDrain)
    ->Name("drogon_burst_feed_drain")
    ->Iterations(kIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaLargePayloadBoundaryFeedDecode)
    ->Name("aquila_large_payload_boundary_feed_decode")
    ->Iterations(kLargeIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkAquilaLinearLargePayloadBoundaryFeedDecode)
    ->Name("aquila_linear_large_payload_boundary_feed_decode")
    ->Iterations(kLargeIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkDrogonLargePayloadBoundaryFeedDecode)
    ->Name("drogon_large_payload_boundary_feed_decode")
    ->Iterations(kLargeIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
