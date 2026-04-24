#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/prepared_write.h"

#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

void BenchmarkPreparedWrite(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  PreparedWriteArena arena(2048, 4096);
  std::uintptr_t fingerprint = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      auto* write = arena.TryAcquire();
      if (write == nullptr) {
        state.SkipWithError("prepared write slot unavailable");
        return;
      }
      benchmark::DoNotOptimize(write);
      fingerprint += reinterpret_cast<std::uintptr_t>(write);
      arena.Release(write);
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "fingerprint", fingerprint);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkPreparedWrite)
    ->Name("prepared_write")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
