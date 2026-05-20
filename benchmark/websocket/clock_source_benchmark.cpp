#include <chrono>
#include <cstdint>
#include <ctime>
#include <vector>

#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/runtime_clock.h"

#if defined(__linux__)
#include <time.h>
#endif

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

void BM_TimeNull(benchmark::State& state) {
  for (auto _ : state) {
    std::time_t now = std::time(nullptr);
    benchmark::DoNotOptimize(now);
  }
  state.SetItemsProcessed(state.iterations());
}

#if defined(__linux__)
void BenchmarkClockGetTime(benchmark::State& state, clockid_t clock_id) {
  timespec ts{};
  for (auto _ : state) {
    int rc = ::clock_gettime(clock_id, &ts);
    benchmark::DoNotOptimize(rc);
    benchmark::DoNotOptimize(ts);
  }
  state.SetItemsProcessed(state.iterations());
}
#endif

void BM_SteadyClockNow(benchmark::State& state) {
  for (auto _ : state) {
    auto now = std::chrono::steady_clock::now();
    benchmark::DoNotOptimize(now);
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_HighResolutionClockNow(benchmark::State& state) {
  for (auto _ : state) {
    auto now = std::chrono::high_resolution_clock::now();
    benchmark::DoNotOptimize(now);
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_TimeNull)->Name("time_null")->Unit(benchmark::kNanosecond);

#if defined(__linux__)
BENCHMARK_CAPTURE(BenchmarkClockGetTime, realtime, CLOCK_REALTIME)
    ->Name("clock_gettime_realtime")
    ->Unit(benchmark::kNanosecond);
BENCHMARK_CAPTURE(BenchmarkClockGetTime, realtime_coarse, CLOCK_REALTIME_COARSE)
    ->Name("clock_gettime_realtime_coarse")
    ->Unit(benchmark::kNanosecond);
BENCHMARK_CAPTURE(BenchmarkClockGetTime, monotonic, CLOCK_MONOTONIC)
    ->Name("clock_gettime_monotonic")
    ->Unit(benchmark::kNanosecond);
BENCHMARK_CAPTURE(BenchmarkClockGetTime, monotonic_coarse,
                  CLOCK_MONOTONIC_COARSE)
    ->Name("clock_gettime_monotonic_coarse")
    ->Unit(benchmark::kNanosecond);
#endif

BENCHMARK(BM_SteadyClockNow)
    ->Name("steady_clock_now")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_HighResolutionClockNow)
    ->Name("high_resolution_clock_now")
    ->Unit(benchmark::kNanosecond);

}  // namespace
