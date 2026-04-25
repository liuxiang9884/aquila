#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/frame_codec.h"
#include "third_party/websocket/websocket.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

}  // namespace
