#include "benchmark/websocket/benchmark_support.h"
#include "core/websocket/active_spin_loop.h"
#include "core/websocket/runtime_policy.h"

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

class FakeSession {
 public:
  void DriveRead() noexcept { ++iterations_; }
  void DriveWrite() noexcept {}
  void AdvanceHeartbeat(std::uint64_t) noexcept {}
  bool ShouldReconnect() const noexcept { return iterations_ >= 1024; }
  std::uint64_t iterations() const noexcept { return iterations_; }

 private:
  std::uint64_t iterations_{0};
};

}  // namespace

int main() {
  constexpr size_t kSamples = 2048;
  RuntimePolicy policy{};
  policy.affinity_mode = AffinityMode::kNone;
  ActiveSpinLoop loop(policy);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kSamples);
  std::uint64_t total_iterations = 0;

  for (size_t sample_index = 0; sample_index < kSamples; ++sample_index) {
    FakeSession session;
    const auto start = std::chrono::steady_clock::now();
    loop.Run(session);
    const auto stop = std::chrono::steady_clock::now();
    if (stop <= start) {
      return 1;
    }
    total_iterations += session.iterations();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                stop - start)
                                .count();
    samples_ns.push_back(static_cast<std::uint64_t>(elapsed_ns));
  }

  PrintReport("active_spin", std::move(samples_ns), false,
              "local-microbenchmark", "iterations", total_iterations);
  return 0;
}
