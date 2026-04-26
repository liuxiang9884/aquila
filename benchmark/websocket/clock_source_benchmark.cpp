#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/runtime_clock.h"

#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;

namespace {

void BenchmarkClockSource(benchmark::State& state, ClockSource source) {
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(static_cast<size_t>(state.iterations()));
  for (auto _ : state) {
    const auto start = benchmarking::NowNs();
    benchmark::DoNotOptimize(NowNs(source));
    samples_ns.push_back(benchmarking::NowNs() - start);
  }
  benchmarking::SetLatencyCounters(state, std::move(samples_ns), "calls",
                                   state.iterations());
}

BENCHMARK_CAPTURE(BenchmarkClockSource, steady, ClockSource::kSteady)
    ->Name("clock_source_steady")
    ->Iterations(8192)
    ->Unit(benchmark::kNanosecond);

BENCHMARK_CAPTURE(BenchmarkClockSource, monotonic, ClockSource::kMonotonic)
    ->Name("clock_source_monotonic")
    ->Iterations(8192)
    ->Unit(benchmark::kNanosecond);

BENCHMARK_CAPTURE(BenchmarkClockSource, monotonic_coarse,
                  ClockSource::kMonotonicCoarse)
    ->Name("clock_source_monotonic_coarse")
    ->Iterations(8192)
    ->Unit(benchmark::kNanosecond);

}  // namespace
