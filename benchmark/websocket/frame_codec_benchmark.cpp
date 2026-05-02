#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>
#include <openssl/rand.h>

#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/frame_codec.h"
#include "evaluation/websocket/queued_frame_codec.h"

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;
using aquila::websocket::evaluation::QueuedFrameCodec;

namespace {

constexpr size_t kCoalescedFrameCount = 16;
constexpr size_t kMaskPoolKeyCount = 4096;

class BenchmarkMaskKeyPool {
 public:
  bool Refill() noexcept {
    if (RAND_bytes(reinterpret_cast<unsigned char*>(keys_.data()),
                   static_cast<int>(keys_.size() * sizeof(keys_[0]))) != 1) {
      return false;
    }
    cursor_ = 0;
    ++refills_;
    return true;
  }

  bool NextWithRefill(std::array<std::byte, 4>* mask_key) noexcept {
    if (cursor_ == keys_.size() && !Refill()) {
      return false;
    }
    *mask_key = keys_[cursor_++];
    return true;
  }

  std::array<std::byte, 4> NextCyclic() noexcept {
    const auto key = keys_[cursor_];
    cursor_ = (cursor_ + 1) & (keys_.size() - 1U);
    return key;
  }

  size_t refill_count() const noexcept {
    return refills_;
  }

 private:
  std::array<std::array<std::byte, 4>, kMaskPoolKeyCount> keys_{};
  size_t cursor_{kMaskPoolKeyCount};
  size_t refills_{0};
};

std::vector<std::byte> BuildClientPayload(size_t payload_size) {
  std::vector<std::byte> payload(payload_size);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = std::byte{static_cast<unsigned char>(i & 0xFFU)};
  }
  return payload;
}

void WriteSmallClientHeader(size_t payload_size,
                            const std::array<std::byte, 4>& mask_key,
                            std::span<std::byte> output) noexcept {
  output[0] = std::byte{0x82};
  output[1] = std::byte{static_cast<unsigned char>(0x80U | payload_size)};
  output[2] = mask_key[0];
  output[3] = mask_key[1];
  output[4] = mask_key[2];
  output[5] = mask_key[3];
}

void ApplyMaskScalar(std::span<const std::byte> payload,
                     const std::array<std::byte, 4>& mask_key,
                     std::span<std::byte> output) noexcept {
  for (size_t i = 0; i < payload.size(); ++i) {
    output[i] = payload[i] ^ mask_key[i & 0x3U];
  }
}

std::uint64_t SumBytes(std::span<const std::byte> bytes) noexcept {
  std::uint64_t sum = 0;
  for (const auto byte : bytes) {
    sum += std::to_integer<unsigned int>(byte);
  }
  return sum;
}

std::vector<std::byte> BuildServerTextFrame(std::string_view payload) {
  const size_t header_bytes = payload.size() <= 125 ? 2 : 4;
  std::vector<std::byte> frame(header_bytes + payload.size());
  frame[0] = std::byte{0x81};
  if (payload.size() <= 125) {
    frame[1] = std::byte{static_cast<unsigned char>(payload.size())};
  } else {
    frame[1] = std::byte{126};
    frame[2] =
        std::byte{static_cast<unsigned char>((payload.size() >> 8U) & 0xFFU)};
    frame[3] = std::byte{static_cast<unsigned char>(payload.size() & 0xFFU)};
  }
  for (size_t i = 0; i < payload.size(); ++i) {
    frame[header_bytes + i] = static_cast<std::byte>(payload[i]);
  }
  return frame;
}

std::vector<std::byte> BuildCoalescedServerTextFrames(std::string_view payload,
                                                      size_t frame_count) {
  const auto frame = BuildServerTextFrame(payload);
  std::vector<std::byte> coalesced;
  coalesced.reserve(frame.size() * frame_count);
  for (size_t i = 0; i < frame_count; ++i) {
    coalesced.insert(coalesced.end(), frame.begin(), frame.end());
  }
  return coalesced;
}

void RunFrameCodecEncodePayload(benchmark::State& state, size_t payload_size) {
  constexpr size_t kBatchSize = 64;
  FrameCodec codec(1024);
  const auto payload = BuildClientPayload(payload_size);
  std::array<std::byte, 1024> storage{};
  std::uint64_t bytes_encoded = 0;
  std::uint64_t header_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto encoded = codec.EncodeBinary(payload, storage);
      if (!encoded.ok) {
        state.SkipWithError("frame encode failed");
        return;
      }
      benchmark::DoNotOptimize(storage);
      bytes_encoded += encoded.bytes.size();
      header_accumulator +=
          SumBytes(std::span<const std::byte>(storage.data(), 2));
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

void BenchmarkFrameCodecEncodeSmall32(benchmark::State& state) {
  RunFrameCodecEncodePayload(state, 32);
}

void BenchmarkFrameCodecEncodeSmall64(benchmark::State& state) {
  RunFrameCodecEncodePayload(state, 64);
}

void BenchmarkFrameCodecEncodeSmall128(benchmark::State& state) {
  RunFrameCodecEncodePayload(state, 128);
}

void BenchmarkFrameCodecEncodeMedium512(benchmark::State& state) {
  RunFrameCodecEncodePayload(state, 512);
}

void BenchmarkFrameCodecMaskRngOnly(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  std::array<std::byte, 4> mask_key{};
  std::uint64_t mask_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      if (RAND_bytes(reinterpret_cast<unsigned char*>(mask_key.data()),
                     static_cast<int>(mask_key.size())) != 1) {
        state.SkipWithError("RAND_bytes failed");
        return;
      }
      benchmark::DoNotOptimize(mask_key.data());
      mask_accumulator += SumBytes(mask_key);
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "mask_accumulator",
                     mask_accumulator);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void RunFrameCodecMaskXorOnly(benchmark::State& state, size_t payload_size) {
  constexpr size_t kBatchSize = 64;
  const auto payload = BuildClientPayload(payload_size);
  std::vector<std::byte> output(payload.size());
  const std::array<std::byte, 4> mask_key{std::byte{0x11}, std::byte{0x22},
                                          std::byte{0x33}, std::byte{0x44}};
  std::uint64_t output_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      ApplyMaskScalar(payload, mask_key, output);
      benchmark::DoNotOptimize(output.data());
      output_accumulator +=
          SumBytes(std::span<const std::byte>(output.data(), 1));
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "output_accumulator",
                     output_accumulator);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkFrameCodecMaskXorOnly64(benchmark::State& state) {
  RunFrameCodecMaskXorOnly(state, 64);
}

void BenchmarkFrameCodecMaskXorOnly128(benchmark::State& state) {
  RunFrameCodecMaskXorOnly(state, 128);
}

void BenchmarkFrameCodecHeaderSmall64(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  std::array<std::byte, 6> header{};
  const std::array<std::byte, 4> mask_key{std::byte{0x11}, std::byte{0x22},
                                          std::byte{0x33}, std::byte{0x44}};
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      WriteSmallClientHeader(64, mask_key, header);
      benchmark::DoNotOptimize(header.data());
      benchmark::ClobberMemory();
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "", 0);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkFrameCodecMaskPoolBatchRefillPerKey(benchmark::State& state) {
  BenchmarkMaskKeyPool pool;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    if (!pool.Refill()) {
      state.SkipWithError("mask pool refill failed");
      return;
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_key_ns = static_cast<double>(elapsed_ns) /
                            static_cast<double>(kMaskPoolKeyCount);
    state.SetIterationTime(per_key_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_key_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "refills",
                     static_cast<std::uint64_t>(pool.refill_count()));
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkFrameCodecMaskPoolPopOnly(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  BenchmarkMaskKeyPool pool;
  if (!pool.Refill()) {
    state.SkipWithError("mask pool refill failed");
    return;
  }
  std::uint64_t mask_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto mask_key = pool.NextCyclic();
      benchmark::DoNotOptimize(mask_key.data());
      mask_accumulator += SumBytes(mask_key);
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "mask_accumulator",
                     mask_accumulator);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkFrameCodecMaskPoolPopWithRefill(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  BenchmarkMaskKeyPool pool;
  std::array<std::byte, 4> mask_key{};
  std::uint64_t mask_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      if (!pool.NextWithRefill(&mask_key)) {
        state.SkipWithError("mask pool refill failed");
        return;
      }
      benchmark::DoNotOptimize(mask_key.data());
      mask_accumulator += SumBytes(mask_key);
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "refills",
                     static_cast<std::uint64_t>(pool.refill_count()));
  state.counters["mask_accumulator"] = static_cast<double>(mask_accumulator);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkFrameCodecEncodeSmall64MaskPool(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  BenchmarkMaskKeyPool pool;
  if (!pool.Refill()) {
    state.SkipWithError("mask pool refill failed");
    return;
  }
  const auto payload = BuildClientPayload(64);
  std::array<std::byte, 70> storage{};
  std::uint64_t bytes_encoded = 0;
  std::uint64_t output_accumulator = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto mask_key = pool.NextCyclic();
      WriteSmallClientHeader(payload.size(), mask_key, storage);
      ApplyMaskScalar(payload, mask_key,
                      std::span<std::byte>(storage.data() + 6, payload.size()));
      benchmark::DoNotOptimize(storage.data());
      bytes_encoded += 6 + payload.size();
      output_accumulator +=
          SumBytes(std::span<const std::byte>(storage.data(), 2));
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "bytes_encoded",
                     bytes_encoded + output_accumulator);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkFrameCodecEncode(benchmark::State& state) {
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
      header_accumulator +=
          SumBytes(std::span<const std::byte>(storage.data(), 2));
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

void BenchmarkFrameCodecDecodeContiguous(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  FrameCodec codec(1024, 4096);
  const auto frame = BuildServerTextFrame("tick");
  std::uint64_t payload_bytes = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto decoded = codec.Feed(frame);
      if (decoded.status != DecodeStatus::kMessageReady) {
        state.SkipWithError("frame decode failed");
        return;
      }
      benchmark::DoNotOptimize(decoded.view.payload.data());
      payload_bytes += decoded.view.payload.size();
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "payload_bytes",
                     payload_bytes);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkQueuedFrameCodecDecodeContiguous(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  QueuedFrameCodec codec(1024, 4096, 1024);
  const auto frame = BuildServerTextFrame("tick");
  std::uint64_t payload_bytes = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      const auto decoded = codec.Feed(frame);
      if (decoded.status != DecodeStatus::kMessageReady) {
        state.SkipWithError("queued frame decode failed");
        return;
      }
      benchmark::DoNotOptimize(decoded.view.payload.data());
      payload_bytes += decoded.view.payload.size();
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "payload_bytes",
                     payload_bytes);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkFrameCodecDecodeCoalescedDrain(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  FrameCodec codec(1024, 4096);
  const auto coalesced =
      BuildCoalescedServerTextFrames("tick", kCoalescedFrameCount);
  std::uint64_t payload_bytes = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      auto decoded = codec.Feed(coalesced);
      size_t decoded_frames = 0;
      while (decoded.status == DecodeStatus::kMessageReady) {
        benchmark::DoNotOptimize(decoded.view.payload.data());
        payload_bytes += decoded.view.payload.size();
        ++decoded_frames;
        decoded = codec.Poll();
      }
      if (decoded.status != DecodeStatus::kNeedMore ||
          decoded_frames != kCoalescedFrameCount) {
        state.SkipWithError("frame codec coalesced drain failed");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kCoalescedFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "payload_bytes",
                     payload_bytes);
  state.counters["frames_per_read"] = static_cast<double>(kCoalescedFrameCount);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkQueuedFrameCodecDecodeCoalescedDrain(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  QueuedFrameCodec codec(1024, 4096, 1024);
  const auto coalesced =
      BuildCoalescedServerTextFrames("tick", kCoalescedFrameCount);
  std::uint64_t payload_bytes = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      auto decoded = codec.Feed(coalesced);
      size_t decoded_frames = 0;
      while (decoded.status == DecodeStatus::kMessageReady) {
        benchmark::DoNotOptimize(decoded.view.payload.data());
        payload_bytes += decoded.view.payload.size();
        ++decoded_frames;
        decoded = codec.Poll();
      }
      if (decoded.status != DecodeStatus::kNeedMore ||
          decoded_frames != kCoalescedFrameCount) {
        state.SkipWithError("queued frame codec coalesced drain failed");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) /
        static_cast<double>(kBatchSize * kCoalescedFrameCount);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "payload_bytes",
                     payload_bytes);
  state.counters["frames_per_read"] = static_cast<double>(kCoalescedFrameCount);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void AlignCodecNearMirroredBoundary(FrameCodec& codec,
                                    const std::vector<std::byte>& filler) {
  codec.Reset();
  const size_t target_offset = codec.ReceiveCapacity() - 5U;
  const size_t filler_count = target_offset / filler.size();
  for (size_t i = 0; i < filler_count; ++i) {
    const auto decoded = codec.Feed(filler);
    benchmark::DoNotOptimize(decoded.view.payload.data());
    benchmark::DoNotOptimize(codec.Poll().status);
  }
}

void AlignCodecNearMirroredBoundary(QueuedFrameCodec& codec,
                                    const std::vector<std::byte>& filler) {
  codec.Reset();
  const size_t target_offset = codec.ReceiveCapacity() - 5U;
  const size_t filler_count = target_offset / filler.size();
  for (size_t i = 0; i < filler_count; ++i) {
    const auto decoded = codec.Feed(filler);
    benchmark::DoNotOptimize(decoded.view.payload.data());
    benchmark::DoNotOptimize(codec.Poll().status);
  }
}

void BenchmarkFrameCodecDecodeMirroredBoundary(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  FrameCodec codec(128, 4096);
  const auto filler = BuildServerTextFrame("x");
  const auto wrapped = BuildServerTextFrame("abcdef");
  std::uint64_t payload_bytes = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    std::uint64_t elapsed_ns = 0;
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      AlignCodecNearMirroredBoundary(codec, filler);
      const std::uint64_t start_ns = NowNs();
      auto writable = codec.WritableSpan();
      if (writable.size() < wrapped.size()) {
        state.SkipWithError("insufficient mirrored writable span");
        return;
      }
      std::copy(wrapped.begin(), wrapped.end(), writable.begin());
      codec.CommitWritten(wrapped.size());
      const auto decoded = codec.Poll();
      if (decoded.status != DecodeStatus::kMessageReady) {
        state.SkipWithError("mirrored boundary decode failed");
        return;
      }
      benchmark::DoNotOptimize(decoded.view.payload.data());
      payload_bytes += decoded.view.payload.size();
      elapsed_ns += NowNs() - start_ns;
    }
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "payload_bytes",
                     payload_bytes);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkQueuedFrameCodecDecodeMirroredBoundary(benchmark::State& state) {
  constexpr size_t kBatchSize = 64;
  QueuedFrameCodec codec(128, 4096, 1024);
  const auto filler = BuildServerTextFrame("x");
  const auto wrapped = BuildServerTextFrame("abcdef");
  std::uint64_t payload_bytes = 0;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    std::uint64_t elapsed_ns = 0;
    for (size_t batch_index = 0; batch_index < kBatchSize; ++batch_index) {
      AlignCodecNearMirroredBoundary(codec, filler);
      const std::uint64_t start_ns = NowNs();
      auto writable = codec.WritableSpan();
      if (writable.size() < wrapped.size()) {
        state.SkipWithError("insufficient queued writable span");
        return;
      }
      std::copy(wrapped.begin(), wrapped.end(), writable.begin());
      codec.CommitWritten(wrapped.size());
      const auto decoded = codec.Poll();
      if (decoded.status != DecodeStatus::kMessageReady) {
        state.SkipWithError("queued boundary decode failed");
        return;
      }
      benchmark::DoNotOptimize(decoded.view.payload.data());
      payload_bytes += decoded.view.payload.size();
      elapsed_ns += NowNs() - start_ns;
    }
    const auto per_operation_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_operation_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_operation_ns));
  }

  SetLatencyCounters(state, std::move(samples_ns), "payload_bytes",
                     payload_bytes);
  state.SetLabel(BuildBenchmarkLabel(false, "local-microbenchmark",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkFrameCodecEncode)
    ->Name("frame_codec_encode")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecEncodeSmall32)
    ->Name("frame_codec_encode_small_32")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecEncodeSmall64)
    ->Name("frame_codec_encode_small_64")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecEncodeSmall128)
    ->Name("frame_codec_encode_small_128")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecEncodeMedium512)
    ->Name("frame_codec_encode_medium_512")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecMaskRngOnly)
    ->Name("frame_codec_mask_rng_only")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecMaskXorOnly64)
    ->Name("frame_codec_mask_xor_only_64")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecMaskXorOnly128)
    ->Name("frame_codec_mask_xor_only_128")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecHeaderSmall64)
    ->Name("frame_codec_header_small_64")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecMaskPoolBatchRefillPerKey)
    ->Name("frame_codec_mask_pool_batch_refill_per_key")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecMaskPoolPopOnly)
    ->Name("frame_codec_mask_pool_pop_only")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecMaskPoolPopWithRefill)
    ->Name("frame_codec_mask_pool_pop_with_refill")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecEncodeSmall64MaskPool)
    ->Name("frame_codec_encode_small_64_mask_pool")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecDecodeContiguous)
    ->Name("frame_codec_decode_contiguous")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkQueuedFrameCodecDecodeContiguous)
    ->Name("queued_frame_codec_decode_contiguous")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecDecodeCoalescedDrain)
    ->Name("frame_codec_decode_coalesced_drain")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkQueuedFrameCodecDecodeCoalescedDrain)
    ->Name("queued_frame_codec_decode_coalesced_drain")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkFrameCodecDecodeMirroredBoundary)
    ->Name("frame_codec_decode_mirrored_boundary")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkQueuedFrameCodecDecodeMirroredBoundary)
    ->Name("queued_frame_codec_decode_mirrored_boundary")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
