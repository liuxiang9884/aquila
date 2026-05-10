#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "core/base/double_heap.h"
#include "core/base/heap_buffer.h"
#include "core/base/histogram_quantile.h"
#include "core/base/monotonic_deque.h"
#include "core/base/ring_queue.h"

namespace {

TEST(MonotonicDequeTest, KeepsMinimumCandidatesAndRetainsEqualValues) {
  aquila::MonotonicDeque<int, std::less<int>> deque;
  deque.Reserve(4);

  deque.Push(5);
  deque.Push(3);
  deque.Push(3);
  deque.Push(4);

  EXPECT_EQ(deque.size(), 3U);
  EXPECT_EQ(deque.Front(), 3);
  EXPECT_EQ(deque.Back(), 4);

  deque.PopFront();
  EXPECT_EQ(deque.Front(), 3);
  EXPECT_EQ(deque.size(), 2U);
}

TEST(MonotonicDequeTest, CompactsBeforeGrowingWhenFrontSpaceExists) {
  aquila::MonotonicDeque<int, std::less<int>> deque;
  deque.Reserve(3);

  deque.Push(1);
  deque.Push(2);
  deque.Push(3);
  const std::size_t capacity = deque.capacity();

  deque.PopFront();
  deque.Push(4);

  EXPECT_EQ(deque.capacity(), capacity);
  EXPECT_EQ(deque.Front(), 2);
  EXPECT_EQ(deque.Back(), 4);
}

TEST(RingQueueTest, RoundsCapacityToPowerOfTwoAndPreservesFifoOnGrow) {
  aquila::RingQueue<int> queue;
  queue.Init(3);

  EXPECT_EQ(queue.capacity(), 4U);

  queue.PushBack(1);
  queue.PushBack(2);
  queue.PushBack(3);
  EXPECT_EQ(queue.PopFront(), 1);
  queue.PushBack(4);
  queue.PushBack(5);
  queue.PushBack(6);

  EXPECT_EQ(queue.capacity(), 8U);
  EXPECT_EQ(queue.size(), 5U);
  EXPECT_EQ(queue[0], 2);
  EXPECT_EQ(queue[4], 6);

  std::vector<int> values;
  while (!queue.empty()) {
    values.push_back(queue.PopFront());
  }
  EXPECT_EQ(values, (std::vector<int>{2, 3, 4, 5, 6}));
}

TEST(HeapBufferTest, SupportsMaxAndMinHeapModes) {
  aquila::HeapBuffer<int, std::less<int>> max_heap;
  max_heap.Reserve(3);
  max_heap.Push(4);
  max_heap.Push(7);
  max_heap.Push(1);
  EXPECT_EQ(max_heap.Top(), 7);
  EXPECT_EQ(max_heap.PopTop(), 7);
  EXPECT_EQ(max_heap.Top(), 4);

  aquila::HeapBuffer<int, std::greater<int>> min_heap;
  min_heap.Reserve(3);
  min_heap.Push(4);
  min_heap.Push(7);
  min_heap.Push(1);
  EXPECT_EQ(min_heap.Top(), 1);
  EXPECT_EQ(min_heap.PopTop(), 1);
  EXPECT_EQ(min_heap.Top(), 4);
}

TEST(DoubleHeapTest, MatchesEmpiricalQuantileForUnsortedInput) {
  aquila::DoubleHeap<double> quantile;
  quantile.Init(0.6, 8);

  for (const double value : {5.0, 1.0, 9.0, 3.0, 4.0}) {
    quantile.Add(value);
  }

  EXPECT_TRUE(quantile.HasValue());
  EXPECT_DOUBLE_EQ(quantile.Value(), 4.0);
}

TEST(DoubleHeapTest, ResetKeepsCapacityAndReturnsEmptyDefault) {
  aquila::DoubleHeap<double> quantile;
  quantile.Init(0.99, 4);
  quantile.Add(1.0);
  quantile.Add(2.0);
  const std::size_t lower_capacity = quantile.lower_capacity();
  const std::size_t upper_capacity = quantile.upper_capacity();

  quantile.Reset();

  EXPECT_FALSE(quantile.HasValue());
  EXPECT_DOUBLE_EQ(quantile.Value(), 0.0);
  EXPECT_EQ(quantile.lower_capacity(), lower_capacity);
  EXPECT_EQ(quantile.upper_capacity(), upper_capacity);
}

TEST(HistogramQuantileTest, EstimatesQuantileWithinConfiguredBinWidth) {
  aquila::HistogramQuantile<double> quantile;
  quantile.Init(0.0, 1.0, 10, 0.6,
                aquila::HistogramQuantileValueMode::kMidpoint);

  for (const double value :
       {0.05, 0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85, 0.95}) {
    quantile.Add(value);
  }

  EXPECT_TRUE(quantile.HasValue());
  EXPECT_LE(std::abs(quantile.Value() - 0.55), quantile.bin_width() / 2.0);
}

TEST(HistogramQuantileTest, TracksOverflowAndDoesNotGrowCounts) {
  aquila::HistogramQuantile<double> quantile;
  quantile.Init(0.0, 1.0, 8, 0.5);
  const std::size_t capacity = quantile.counts_capacity();

  quantile.Add(-0.1);
  quantile.Add(1.1);
  quantile.Add(0.4);

  EXPECT_EQ(quantile.underflow_count(), 1U);
  EXPECT_EQ(quantile.overflow_count(), 1U);
  EXPECT_EQ(quantile.counts_capacity(), capacity);
}

TEST(HistogramQuantileTest, UsesDefaultBinCountWhenBinCountIsOmitted) {
  aquila::HistogramQuantile<double> quantile;
  quantile.Init(900.0, 1100.0, 0.6,
                aquila::HistogramQuantileValueMode::kMidpoint);

  EXPECT_EQ(quantile.bin_count(),
            aquila::HistogramQuantile<double>::kDefaultBinCount);
}

TEST(HistogramQuantileTest, ComputesExactBinsFromReferenceErrorBp) {
  aquila::HistogramQuantile<double> quantile;
  quantile.InitWithReferenceError(
      900.0, 1100.0, 1000.0, 0.1, 0.6,
      aquila::HistogramQuantileValueMode::kMidpoint);

  EXPECT_EQ(quantile.bin_count(), 10000U);
  EXPECT_LE(quantile.bin_width(), 0.02);
}

TEST(HistogramQuantileTest, ComputesBinsFromRangePrecision) {
  aquila::HistogramQuantile<double> quantile;
  quantile.InitWithRangePrecision(
      0.0, 0.02, 0.00001, 0.6, aquila::HistogramQuantileValueMode::kMidpoint);

  EXPECT_EQ(quantile.bin_count(), 1000U);
  EXPECT_DOUBLE_EQ(quantile.bin_width(), 0.00002);
}

TEST(HistogramQuantileTest, Avx2ValueMatchesScalarValue) {
  aquila::HistogramQuantile<double> quantile;
  quantile.Init(0.0, 1.0, 17, 0.6,
                aquila::HistogramQuantileValueMode::kMidpoint);

  for (const double value : {0.01, 0.02, 0.11, 0.12, 0.13, 0.21, 0.31, 0.41,
                             0.42, 0.51, 0.61, 0.62, 0.71, 0.81, 0.91}) {
    quantile.Add(value);
  }

  EXPECT_DOUBLE_EQ(quantile.ValueScalar(), quantile.ValueAvx2());
}

}  // namespace
