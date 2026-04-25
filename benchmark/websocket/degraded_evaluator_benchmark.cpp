#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/degraded_evaluator.h"

#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

void BenchmarkDegradedEvaluator(benchmark::State& state) {
  DegradedThresholds thresholds{};
  thresholds.high_watermark_percent = 80;
  thresholds.high_watermark_hold_ticks = 8;
  thresholds.recover_ticks = 16;
  thresholds.backpressure_drops_per_second = 10;
  thresholds.awaiting_pong_timeout_ms = 3000;

  DegradedEvaluator evaluator(thresholds);
  DegradedSample sample{};
  sample.now_ns = 1'000'000'000ULL;
  sample.prepared_write_slots = 2048;
  sample.pending_write_count = 64;
  sample.last_ping_ns = sample.now_ns;

  constexpr std::uint64_t kInnerIterations = 1024;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t evaluations = 0;

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (std::uint64_t i = 0; i < kInnerIterations; ++i) {
      sample.now_ns += 1'000'000ULL;
      sample.pending_write_count = (i & 7U) == 0 ? 1600 : 64;
      sample.consumer_backpressure_drops += (i & 31U) == 0 ? 1 : 0;
      sample.awaiting_pong = (i & 63U) == 63U;
      benchmark::DoNotOptimize(evaluator.Evaluate(sample));
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const std::uint64_t per_evaluation_ns =
        elapsed_ns / kInnerIterations;
    state.SetIterationTime(
        static_cast<double>(per_evaluation_ns) / 1'000'000'000.0);
    samples_ns.push_back(per_evaluation_ns);
    evaluations += kInnerIterations;
  }

  SetLatencyCounters(state, std::move(samples_ns), "evaluations",
                     evaluations);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkDegradedEvaluator)
    ->Name("degraded_evaluator")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
