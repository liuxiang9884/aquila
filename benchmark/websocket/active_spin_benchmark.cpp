#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/active_spin_loop.h"
#include "core/websocket/runtime_policy.h"

#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

class FakeSession {
 public:
  void DriveRead() noexcept { ++iterations_; }
  void DriveWrite() noexcept {}
  void AdvanceHeartbeat(std::uint64_t) noexcept {}
  bool ShouldReconnect() const noexcept { return iterations_ >= 1; }
  std::uint64_t iterations() const noexcept { return iterations_; }

 private:
  std::uint64_t iterations_{0};
};

void BenchmarkActiveSpin(benchmark::State& state) {
  RuntimePolicy policy{};
  policy.affinity_mode = AffinityMode::kNone;
  ActiveSpinLoop loop(policy);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(8192);
  std::uint64_t total_iterations = 0;

  for (auto _ : state) {
    FakeSession session;
    const std::uint64_t start_ns = NowNs();
    loop.Run(session);
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    total_iterations += session.iterations();
    samples_ns.push_back(elapsed_ns);
  }

  SetLatencyCounters(state, std::move(samples_ns), "iterations",
                     total_iterations);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkActiveSpin)
    ->Name("active_spin")
    ->Iterations(8192)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
