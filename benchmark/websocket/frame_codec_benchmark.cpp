#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/frame_codec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

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

void BenchmarkFrameCodecEncode(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  FrameCodec codec(1024);
  std::array<std::byte, 128> storage{};
  std::uint64_t bytes_encoded = 0;
  std::uint64_t header_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto encoded =
          codec.EncodeText(std::as_bytes(std::span{"tick", 4}), storage);
      if (!encoded.ok) {
        state.SkipWithError("frame encode failed");
        return;
      }
      benchmark::DoNotOptimize(storage);
      bytes_encoded += encoded.bytes.size();
      header_accumulator += static_cast<std::uint8_t>(storage[0]) +
                            static_cast<std::uint8_t>(storage[1]);
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "bytes_encoded",
                     bytes_encoded + header_accumulator);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkFrameCodecDecodeContiguous(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  FrameCodec codec(1024, 4096, 1024);
  const auto frame = BuildServerTextFrame("tick");
  std::uint64_t payload_bytes = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto decoded = codec.Feed(frame);
      if (decoded.status != DecodeStatus::kMessageReady) {
        state.SkipWithError("frame decode failed");
        return;
      }
      benchmark::DoNotOptimize(decoded.view.payload.data());
      payload_bytes += decoded.view.payload.size();
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "payload_bytes",
                     payload_bytes);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void AlignCodecNearMirroredBoundary(FrameCodec& codec,
                                    const std::vector<std::byte>& filler) {
  codec.Reset();
  const size_t target_offset = codec.ReceiveCapacity() - 5U;
  const size_t filler_count = target_offset / filler.size();
  for (size_t i = 0; i < filler_count; ++i) {
    const auto decoded = codec.Feed(filler);
    benchmark::DoNotOptimize(decoded.view.payload.data());
    benchmark::DoNotOptimize(codec.Poll().status);
  }
}

void BenchmarkFrameCodecDecodeMirroredBoundary(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  FrameCodec codec(128, 4096, 1024);
  const auto filler = BuildServerTextFrame("x");
  const auto wrapped = BuildServerTextFrame("abcdef");
  std::uint64_t payload_bytes = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    std::uint64_t elapsed_ns = 0;
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      AlignCodecNearMirroredBoundary(codec, filler);
      const std::uint64_t start_ns = NowNs();
      auto writable = codec.WritableSpan();
      if (writable.size() < wrapped.size()) {
        state.SkipWithError("insufficient mirrored writable span");
        return;
      }
      std::copy(wrapped.begin(), wrapped.end(), writable.begin());
      codec.CommitWritten(wrapped.size());
      const auto decoded = codec.Poll();
      if (decoded.status != DecodeStatus::kMessageReady) {
        state.SkipWithError("mirrored boundary decode failed");
        return;
      }
      benchmark::DoNotOptimize(decoded.view.payload.data());
      payload_bytes += decoded.view.payload.size();
      elapsed_ns += NowNs() - start_ns;
    }
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "payload_bytes",
                     payload_bytes);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkFrameCodecEncode)
    ->Name("frame_codec_encode")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecDecodeContiguous)
    ->Name("frame_codec_decode_contiguous")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecDecodeMirroredBoundary)
    ->Name("frame_codec_decode_mirrored_boundary")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
