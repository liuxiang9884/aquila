#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <benchmark/benchmark.h>

#include "core/base/double_heap.h"
#include "core/base/heap_buffer.h"
#include "core/base/histogram_quantile.h"
#include "core/base/monotonic_deque.h"
#include "core/base/ring_queue.h"

namespace aquila {
namespace {

constexpr double kQuantile = 0.6;
constexpr double kHistogramMin = 0.0;
constexpr double kHistogramMax = 0.01;
constexpr std::size_t kDefaultSamples = 10000;
constexpr std::size_t kDefaultBins = 4096;

std::vector<double> MakeSamples(std::size_t count) {
  std::vector<double> samples;
  samples.reserve(count);
  std::uint64_t state = 0x9E3779B97F4A7C15ULL;
  for (std::size_t i = 0; i < count; ++i) {
    state = state * 2862933555777941757ULL + 3037000493ULL;
    const double unit =
        static_cast<double>((state >> 16) % 1'000'000ULL) / 1'000'000.0;
    samples.push_back(kHistogramMin + unit * (kHistogramMax - kHistogramMin));
  }
  return samples;
}

double ExactEmpiricalQuantile(std::vector<double> samples, double quantile) {
  std::sort(samples.begin(), samples.end());
  const std::size_t count = samples.size();
  std::size_t rank = static_cast<std::size_t>(std::ceil(quantile * count));
  rank = std::max<std::size_t>(1, rank);
  rank = std::min(rank, count);
  return samples[rank - 1];
}

void BM_MonotonicDequePushNoGrow(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    MonotonicDeque<double, std::less<double>> deque;
    deque.Reserve(count);
    state.ResumeTiming();

    for (std::size_t i = 0; i < count; ++i) {
      deque.Push(static_cast<double>(i));
    }
    double front = deque.Front();
    double back = deque.Back();
    benchmark::DoNotOptimize(front);
    benchmark::DoNotOptimize(back);
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(count);
  }
  state.SetItemsProcessed(processed);
}

void BM_MonotonicDequePushGrow(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    MonotonicDeque<double, std::less<double>> deque;
    deque.Reserve(1);
    state.ResumeTiming();

    for (std::size_t i = 0; i < count; ++i) {
      deque.Push(static_cast<double>(i));
    }
    double front = deque.Front();
    double back = deque.Back();
    benchmark::DoNotOptimize(front);
    benchmark::DoNotOptimize(back);
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(count);
  }
  state.SetItemsProcessed(processed);
}

void BM_RingQueuePushNoGrow(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    RingQueue<double> queue(count);
    state.ResumeTiming();

    for (std::size_t i = 0; i < count; ++i) {
      queue.PushBack(static_cast<double>(i));
    }
    benchmark::DoNotOptimize(queue.Front());
    benchmark::DoNotOptimize(queue.size());
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(count);
  }
  state.SetItemsProcessed(processed);
}

void BM_RingQueuePushGrow(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    RingQueue<double> queue(1);
    state.ResumeTiming();

    for (std::size_t i = 0; i < count; ++i) {
      queue.PushBack(static_cast<double>(i));
    }
    benchmark::DoNotOptimize(queue.Front());
    benchmark::DoNotOptimize(queue.size());
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(count);
  }
  state.SetItemsProcessed(processed);
}

void BM_HeapBufferPushNoGrow(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    HeapBuffer<double, std::less<double>> heap;
    heap.Reserve(count);
    state.ResumeTiming();

    for (std::size_t i = 0; i < count; ++i) {
      heap.Push(static_cast<double>((i * 17U) % count));
    }
    double top = heap.Top();
    benchmark::DoNotOptimize(top);
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(count);
  }
  state.SetItemsProcessed(processed);
}

void BM_HeapBufferPushGrow(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    HeapBuffer<double, std::less<double>> heap;
    heap.Reserve(1);
    state.ResumeTiming();

    for (std::size_t i = 0; i < count; ++i) {
      heap.Push(static_cast<double>((i * 17U) % count));
    }
    double top = heap.Top();
    benchmark::DoNotOptimize(top);
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(count);
  }
  state.SetItemsProcessed(processed);
}

void BM_DoubleHeapAddNoGrow(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  const std::vector<double> samples = MakeSamples(count);
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    DoubleHeap<double> quantile;
    quantile.Init(kQuantile, count);
    state.ResumeTiming();

    for (double value : samples) {
      quantile.Add(value);
    }
    benchmark::DoNotOptimize(quantile.Value());
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(count);
  }
  state.SetItemsProcessed(processed);
}

void BM_DoubleHeapAddGrow(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  const std::vector<double> samples = MakeSamples(count);
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    DoubleHeap<double> quantile;
    quantile.Init(kQuantile, 1);
    state.ResumeTiming();

    for (double value : samples) {
      quantile.Add(value);
    }
    benchmark::DoNotOptimize(quantile.Value());
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(count);
  }
  state.SetItemsProcessed(processed);
}

void BM_DoubleHeapQuantileBuildAndRead(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  const std::vector<double> samples = MakeSamples(count);
  const double exact = ExactEmpiricalQuantile(samples, kQuantile);
  double last_value = 0.0;
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    DoubleHeap<double> quantile;
    quantile.Init(kQuantile, count);
    state.ResumeTiming();

    for (double value : samples) {
      quantile.Add(value);
    }
    last_value = quantile.Value();
    benchmark::DoNotOptimize(last_value);
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(count);
  }
  state.SetItemsProcessed(processed);
  state.counters["abs_error_bp"] = std::abs(last_value - exact) * 10000.0;
}

void BM_HistogramQuantileBuildAndRead(benchmark::State& state) {
  const auto count = static_cast<std::size_t>(state.range(0));
  const auto bins = static_cast<std::size_t>(state.range(1));
  const std::vector<double> samples = MakeSamples(count);
  const double exact = ExactEmpiricalQuantile(samples, kQuantile);
  double last_value = 0.0;
  double bin_width = 0.0;
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    HistogramQuantile<double> quantile;
    quantile.Init(kHistogramMin, kHistogramMax, bins, kQuantile,
                  HistogramQuantileValueMode::kMidpoint);
    bin_width = quantile.bin_width();
    state.ResumeTiming();

    for (double value : samples) {
      quantile.Add(value);
    }
    last_value = quantile.Value();
    benchmark::DoNotOptimize(last_value);
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(count);
  }
  state.SetItemsProcessed(processed);
  state.counters["abs_error_bp"] = std::abs(last_value - exact) * 10000.0;
  state.counters["bin_width_bp"] = bin_width * 10000.0;
}

BENCHMARK(BM_MonotonicDequePushNoGrow)
    ->Arg(static_cast<std::int64_t>(kDefaultSamples))
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_MonotonicDequePushGrow)
    ->Arg(static_cast<std::int64_t>(kDefaultSamples))
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_RingQueuePushNoGrow)
    ->Arg(static_cast<std::int64_t>(kDefaultSamples))
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_RingQueuePushGrow)
    ->Arg(static_cast<std::int64_t>(kDefaultSamples))
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_HeapBufferPushNoGrow)
    ->Arg(static_cast<std::int64_t>(kDefaultSamples))
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_HeapBufferPushGrow)
    ->Arg(static_cast<std::int64_t>(kDefaultSamples))
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_DoubleHeapAddNoGrow)
    ->Arg(static_cast<std::int64_t>(kDefaultSamples))
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_DoubleHeapAddGrow)
    ->Arg(static_cast<std::int64_t>(kDefaultSamples))
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_DoubleHeapQuantileBuildAndRead)
    ->Arg(static_cast<std::int64_t>(kDefaultSamples))
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_HistogramQuantileBuildAndRead)
    ->Args({static_cast<std::int64_t>(kDefaultSamples),
            static_cast<std::int64_t>(kDefaultBins)})
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila
