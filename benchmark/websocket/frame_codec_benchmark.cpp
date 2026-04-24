#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/frame_codec.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

void BenchmarkFrameCodec(benchmark::State& state) {
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

BENCHMARK(BenchmarkFrameCodec)
    ->Name("frame_codec")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
