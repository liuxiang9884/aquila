#include "benchmark/websocket/benchmark_support.h"
#include "core/websocket/frame_codec.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

int main() {
  constexpr size_t kSamples = 4096;
  constexpr size_t kBatchSize = 64;
  FrameCodec codec(1024);
  std::array<std::byte, 128> storage{};
  std::uint64_t bytes_encoded = 0;
  std::uint64_t header_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kSamples);

  for (size_t sample_index = 0; sample_index < kSamples; ++sample_index) {
    const auto start = std::chrono::steady_clock::now();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto encoded =
          codec.EncodeText(std::as_bytes(std::span{"tick", 4}), storage);
      if (!encoded.ok) {
        return 1;
      }
      bytes_encoded += encoded.bytes.size();
      header_accumulator += static_cast<std::uint8_t>(storage[0]) +
                            static_cast<std::uint8_t>(storage[1]);
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

  PrintReport("frame_codec", std::move(samples_ns), false,
              "local-microbenchmark", "bytes_encoded",
              bytes_encoded + header_accumulator);
  return 0;
}
