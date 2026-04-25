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
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

namespace ws = aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

constexpr size_t kBatchSize = 64;
constexpr size_t kIterations = 4096;
constexpr size_t kCoalescedFrameCount = 16;
constexpr size_t kThirdPartyRecvBufferBytes = 4096;

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
    protocol_error_pending_ = false;
    capacity_error_pending_ = false;
    delivered_pending_ = false;
    delivered_frame_end_ = 0;
  }

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
  bool protocol_error_pending_{false};
  bool capacity_error_pending_{false};
  bool delivered_pending_{false};
  size_t delivered_frame_end_{0};
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

}  // namespace
