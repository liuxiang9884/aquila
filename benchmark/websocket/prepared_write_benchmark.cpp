#include "benchmark/websocket/benchmark_support.h"
#include "core/websocket/prepared_write.h"

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

int main() {
  constexpr size_t kSamples = 4096;
  constexpr size_t kBatchSize = 64;
  PreparedWriteArena arena(2048, 4096);
  std::uintptr_t fingerprint = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kSamples);

  for (size_t sample_index = 0; sample_index < kSamples; ++sample_index) {
    const auto start = std::chrono::steady_clock::now();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      auto* write = arena.TryAcquire();
      if (write == nullptr) {
        return 1;
      }
      fingerprint += reinterpret_cast<std::uintptr_t>(write);
      arena.Release(write);
    }
    const auto stop = std::chrono::steady_clock::now();
    if (stop <= start) {
      return 1;
    }
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                stop - start)
                                .count();
    samples_ns.push_back(static_cast<std::uint64_t>(elapsed_ns / kBatchSize));
  }

  PrintReport("prepared_write", std::move(samples_ns), false,
              "local-microbenchmark", "fingerprint", fingerprint);
  return 0;
}
