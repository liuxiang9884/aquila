#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <vector>

#include <benchmark/benchmark.h>

#include "core/base/double_heap.h"
#include "core/base/heap_buffer.h"
#include "core/base/histogram_quantile.h"
#include "core/base/monotonic_deque.h"
#include "core/base/ring_queue.h"
#include "tools/benchmark/time_series_data.h"

namespace aquila {
namespace {

constexpr double kQuantile = 0.6;
constexpr double kHistogramMin = 0.0;
constexpr double kHistogramMax = 0.01;
constexpr std::size_t kDefaultSamples = 10000;
constexpr std::size_t kDefaultBins = 4096;
constexpr std::uint64_t kTraceWindowNs = 30'000'000'000ULL;
constexpr std::size_t kTraceInitialCapacity = 32768;
constexpr double kTraceHistogramMin = 900.0;
constexpr double kTraceHistogramMax = 1100.0;
constexpr const char* kTracePathEnv = "AQUILA_TIME_SERIES_TRACE";
constexpr const char* kDefaultTracePath =
    "data/benchmark/time_series_1m_1000s_f64.bin";

using TracePoint = tools::benchmark_data::TimeSeriesPoint;

struct TimedValue {
  std::uint64_t timestamp_ns{};
  double value{};
};

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

std::filesystem::path TimeSeriesTracePath() {
  const char* env_value = std::getenv(kTracePathEnv);
  if (env_value != nullptr && env_value[0] != '\0') {
    return std::filesystem::path(env_value);
  }
  return std::filesystem::path(kDefaultTracePath);
}

const std::vector<TracePoint>& TimeSeriesTrace() {
  static const std::vector<TracePoint> trace =
      tools::benchmark_data::ReadTimeSeriesFile(TimeSeriesTracePath());
  return trace;
}

std::uint64_t NextRollAt(std::uint64_t timestamp_ns,
                         std::uint64_t window_ns) noexcept {
  return (timestamp_ns / window_ns) * window_ns + window_ns;
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

void BM_TimeSeriesMonotonicDequeRollingMax(benchmark::State& state) {
  const std::vector<TracePoint>& trace = TimeSeriesTrace();
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    MonotonicDeque<double, std::greater<double>> max_values;
    max_values.Reserve(kTraceInitialCapacity);
    std::size_t head = 0;
    double rolling_max = 0.0;
    state.ResumeTiming();

    for (std::size_t i = 0; i < trace.size(); ++i) {
      const TracePoint& point = trace[i];
      max_values.Push(point.value);
      while (head <= i &&
             point.timestamp_ns - trace[head].timestamp_ns > kTraceWindowNs) {
        if (!max_values.empty() && trace[head].value == max_values.Front()) {
          max_values.PopFront();
        }
        ++head;
      }
      rolling_max = max_values.Front();
      benchmark::DoNotOptimize(rolling_max);
    }
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(trace.size());
  }
  state.SetItemsProcessed(processed);
}

void BM_TimeSeriesRingQueueRollingMean(benchmark::State& state) {
  const std::vector<TracePoint>& trace = TimeSeriesTrace();
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    RingQueue<TimedValue> queue(kTraceInitialCapacity);
    double sum = 0.0;
    double mean = 0.0;
    state.ResumeTiming();

    for (const TracePoint& point : trace) {
      queue.PushBack(
          TimedValue{.timestamp_ns = point.timestamp_ns, .value = point.value});
      sum += point.value;
      while (!queue.empty() &&
             point.timestamp_ns - queue.Front().timestamp_ns > kTraceWindowNs) {
        const TimedValue old = queue.PopFront();
        sum -= old.value;
      }
      mean = sum / static_cast<double>(queue.size());
      benchmark::DoNotOptimize(mean);
    }
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(trace.size());
  }
  state.SetItemsProcessed(processed);
}

void BM_TimeSeriesHeapBufferWindowMax(benchmark::State& state) {
  const std::vector<TracePoint>& trace = TimeSeriesTrace();
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    HeapBuffer<double, std::less<double>> heap;
    heap.Reserve(kTraceInitialCapacity);
    std::uint64_t roll_at =
        trace.empty() ? 0
                      : NextRollAt(trace.front().timestamp_ns, kTraceWindowNs);
    double max_value = 0.0;
    state.ResumeTiming();

    for (const TracePoint& point : trace) {
      if (point.timestamp_ns > roll_at) {
        if (!heap.empty()) {
          max_value = heap.Top();
          benchmark::DoNotOptimize(max_value);
        }
        heap.Clear();
        roll_at = NextRollAt(point.timestamp_ns, kTraceWindowNs);
      }
      heap.Push(point.value);
    }
    if (!heap.empty()) {
      max_value = heap.Top();
      benchmark::DoNotOptimize(max_value);
    }
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(trace.size());
  }
  state.SetItemsProcessed(processed);
}

void BM_TimeSeriesDoubleHeapWindowQuantile(benchmark::State& state) {
  const std::vector<TracePoint>& trace = TimeSeriesTrace();
  std::int64_t processed = 0;
  for (auto _ : state) {
    state.PauseTiming();
    DoubleHeap<double> quantile;
    quantile.Init(kQuantile, kTraceInitialCapacity);
    std::uint64_t roll_at =
        trace.empty() ? 0
                      : NextRollAt(trace.front().timestamp_ns, kTraceWindowNs);
    double value = 0.0;
    state.ResumeTiming();

    for (const TracePoint& point : trace) {
      if (point.timestamp_ns > roll_at) {
        value = quantile.Value();
        benchmark::DoNotOptimize(value);
        quantile.Reset();
        roll_at = NextRollAt(point.timestamp_ns, kTraceWindowNs);
      }
      quantile.Add(point.value);
    }
    value = quantile.Value();
    benchmark::DoNotOptimize(value);
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(trace.size());
  }
  state.SetItemsProcessed(processed);
}

void BM_TimeSeriesHistogramQuantileWindowQuantile(benchmark::State& state) {
  const std::vector<TracePoint>& trace = TimeSeriesTrace();
  std::int64_t processed = 0;
  std::uint64_t last_underflow_count = 0;
  std::uint64_t last_overflow_count = 0;
  double bin_width = 0.0;
  for (auto _ : state) {
    state.PauseTiming();
    HistogramQuantile<double> quantile;
    quantile.Init(kTraceHistogramMin, kTraceHistogramMax, kDefaultBins,
                  kQuantile, HistogramQuantileValueMode::kMidpoint);
    bin_width = quantile.bin_width();
    std::uint64_t roll_at =
        trace.empty() ? 0
                      : NextRollAt(trace.front().timestamp_ns, kTraceWindowNs);
    std::uint64_t underflow_count = 0;
    std::uint64_t overflow_count = 0;
    double value = 0.0;
    state.ResumeTiming();

    for (const TracePoint& point : trace) {
      if (point.timestamp_ns > roll_at) {
        value = quantile.Value();
        benchmark::DoNotOptimize(value);
        underflow_count += quantile.underflow_count();
        overflow_count += quantile.overflow_count();
        quantile.Reset();
        roll_at = NextRollAt(point.timestamp_ns, kTraceWindowNs);
      }
      quantile.Add(point.value);
    }
    value = quantile.Value();
    underflow_count += quantile.underflow_count();
    overflow_count += quantile.overflow_count();
    last_underflow_count = underflow_count;
    last_overflow_count = overflow_count;
    benchmark::DoNotOptimize(value);
    benchmark::ClobberMemory();
    processed += static_cast<std::int64_t>(trace.size());
  }
  state.SetItemsProcessed(processed);
  state.counters["bin_width"] = bin_width;
  state.counters["underflow"] = static_cast<double>(last_underflow_count);
  state.counters["overflow"] = static_cast<double>(last_overflow_count);
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
BENCHMARK(BM_TimeSeriesMonotonicDequeRollingMax)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_TimeSeriesRingQueueRollingMean)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_TimeSeriesHeapBufferWindowMax)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_TimeSeriesDoubleHeapWindowQuantile)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_TimeSeriesHistogramQuantileWindowQuantile)
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila
