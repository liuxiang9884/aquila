#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/active_spin_loop.h"
#include "core/websocket/runtime_policy.h"

#include <atomic>
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

class CurrentStopCheckedSession {
 public:
  void DriveWrite() noexcept {
    if (!stop_requested_.load()) {
      ++operations_;
    }
  }

  void DriveRead() noexcept {
    if (!stop_requested_.load()) {
      ++operations_;
    }
  }

  void AdvanceClock(std::uint64_t, std::uint32_t elapsed_iterations) noexcept {
    if (!stop_requested_.load()) {
      operations_ += elapsed_iterations;
    }
  }

  bool ShouldReconnect() const noexcept {
    return stop_requested_.load(std::memory_order_acquire);
  }

  std::uint64_t operations() const noexcept { return operations_; }

 private:
  std::atomic<bool> stop_requested_{false};
  std::uint64_t operations_{0};
};

class BoundaryStopCheckedSession {
 public:
  void DriveWrite() noexcept { ++operations_; }

  void DriveRead() noexcept { ++operations_; }

  void AdvanceClock(std::uint64_t, std::uint32_t elapsed_iterations) noexcept {
    operations_ += elapsed_iterations;
  }

  bool ShouldReconnect() const noexcept {
    return stop_requested_.load(std::memory_order_acquire);
  }

  std::uint64_t operations() const noexcept { return operations_; }

 private:
  std::atomic<bool> stop_requested_{false};
  std::uint64_t operations_{0};
};

template <typename SessionT>
void RunStopCheckLoop(SessionT& session, std::uint32_t loop_iterations,
                      std::uint32_t clock_interval) noexcept {
  std::uint32_t iterations_since_clock = 0;
  for (std::uint32_t i = 0; i < loop_iterations; ++i) {
    if (session.ShouldReconnect()) {
      return;
    }
    session.DriveWrite();
    session.DriveRead();
    ++iterations_since_clock;
    if (iterations_since_clock >= clock_interval) {
      session.AdvanceClock(0, iterations_since_clock);
      iterations_since_clock = 0;
    }
  }
}

template <typename SessionT>
void BenchmarkStopCheckOverhead(benchmark::State& state) {
  constexpr std::uint32_t kLoopIterations = 1024;
  constexpr std::uint32_t kClockInterval = 4096;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(8192);
  std::uint64_t total_iterations = 0;
  std::uint64_t total_operations = 0;

  for (auto _ : state) {
    SessionT session;
    const std::uint64_t start_ns = NowNs();
    RunStopCheckLoop(session, kLoopIterations, kClockInterval);
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    state.SetIterationTime(
        static_cast<double>(elapsed_ns) / 1'000'000'000.0 /
        static_cast<double>(kLoopIterations));
    total_iterations += kLoopIterations;
    total_operations += session.operations();
    samples_ns.push_back(elapsed_ns / kLoopIterations);
  }

  SetLatencyCounters(state, std::move(samples_ns), "active_iterations",
                     total_iterations);
  state.counters["operations"] = static_cast<double>(total_operations);
  state.SetLabel(BuildBenchmarkLabel(false, "stop-check-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK_TEMPLATE(BenchmarkStopCheckOverhead, CurrentStopCheckedSession)
    ->Name("active_spin_stop_check_current")
    ->Iterations(8192)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK_TEMPLATE(BenchmarkStopCheckOverhead, BoundaryStopCheckedSession)
    ->Name("active_spin_stop_check_boundary_only")
    ->Iterations(8192)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
