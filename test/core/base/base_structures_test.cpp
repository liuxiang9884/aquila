#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "core/base/double_heap.h"
#include "core/base/fixed_ordered_slot_pool.h"
#include "core/base/heap_buffer.h"
#include "core/base/histogram_quantile.h"
#include "core/base/monotonic_deque.h"
#include "core/base/ring_queue.h"

namespace {

struct FixedOrderedSlotPoolTestValue {
  std::uint64_t id{0};
  int payload{0};
};

TEST(FixedOrderedSlotPoolTest,
     InitializesWithClampedCapacityAndRejectsWhenFull) {
  aquila::FixedOrderedSlotPool<FixedOrderedSlotPoolTestValue, 4> slots;

  EXPECT_EQ(slots.Initialize(10), 4U);
  EXPECT_EQ(slots.capacity(), 4U);
  EXPECT_TRUE(slots.empty());

  EXPECT_NE(
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 1, .payload = 10}),
      decltype(slots)::kInvalidIndex);
  EXPECT_NE(
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 2, .payload = 20}),
      decltype(slots)::kInvalidIndex);
  EXPECT_NE(
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 3, .payload = 30}),
      decltype(slots)::kInvalidIndex);
  EXPECT_NE(
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 4, .payload = 40}),
      decltype(slots)::kInvalidIndex);

  EXPECT_TRUE(slots.full());
  EXPECT_EQ(
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 5, .payload = 50}),
      decltype(slots)::kInvalidIndex);
  EXPECT_EQ(slots.active_count(), 4U);
}

TEST(FixedOrderedSlotPoolTest, ReusesLowSlotWithoutChangingFifoOrder) {
  aquila::FixedOrderedSlotPool<FixedOrderedSlotPoolTestValue, 4> slots;
  EXPECT_EQ(slots.Initialize(4), 4U);

  const auto first =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 101, .payload = 1});
  const auto second =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 102, .payload = 2});
  const auto third =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 103, .payload = 3});
  ASSERT_NE(first, decltype(slots)::kInvalidIndex);
  ASSERT_NE(second, decltype(slots)::kInvalidIndex);
  ASSERT_NE(third, decltype(slots)::kInvalidIndex);

  EXPECT_TRUE(slots.Erase(second));
  const auto reused =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 104, .payload = 4});
  EXPECT_EQ(reused, second);

  const auto active = slots.active_indices();
  ASSERT_EQ(active.size(), 3U);
  EXPECT_EQ(active[0], first);
  EXPECT_EQ(active[1], third);
  EXPECT_EQ(active[2], reused);
  EXPECT_EQ(slots.At(active[0]).id, 101U);
  EXPECT_EQ(slots.At(active[1]).id, 103U);
  EXPECT_EQ(slots.At(active[2]).id, 104U);
}

TEST(FixedOrderedSlotPoolTest, EraseHandlesHeadMiddleTailAndInvalidIndex) {
  aquila::FixedOrderedSlotPool<FixedOrderedSlotPoolTestValue, 5> slots;
  EXPECT_EQ(slots.Initialize(5), 5U);

  const auto first =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 1, .payload = 1});
  const auto second =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 2, .payload = 2});
  const auto third =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 3, .payload = 3});
  const auto fourth =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 4, .payload = 4});

  EXPECT_TRUE(slots.Erase(first));
  EXPECT_TRUE(slots.Erase(third));
  EXPECT_TRUE(slots.Erase(fourth));
  EXPECT_FALSE(slots.Erase(fourth));
  EXPECT_FALSE(slots.Erase(decltype(slots)::kInvalidIndex));

  const auto active = slots.active_indices();
  ASSERT_EQ(active.size(), 1U);
  EXPECT_EQ(active[0], second);
  EXPECT_EQ(slots.At(second).id, 2U);
  EXPECT_FALSE(slots.occupied(first));
  EXPECT_TRUE(slots.occupied(second));
}

TEST(FixedOrderedSlotPoolTest, FindIndexIfScansOnlyActiveSlotsInFifoOrder) {
  aquila::FixedOrderedSlotPool<FixedOrderedSlotPoolTestValue, 4> slots;
  EXPECT_EQ(slots.Initialize(4), 4U);

  const auto first =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 10, .payload = 1});
  const auto second =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 20, .payload = 2});
  const auto third =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 30, .payload = 3});
  ASSERT_TRUE(slots.Erase(second));
  const auto fourth =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 40, .payload = 4});

  std::vector<std::uint64_t> visited_ids;
  const auto found =
      slots.FindIndexIf([&](const FixedOrderedSlotPoolTestValue& value) {
        visited_ids.push_back(value.id);
        return value.id == 40;
      });

  EXPECT_EQ(found, fourth);
  EXPECT_EQ(visited_ids, (std::vector<std::uint64_t>{10, 30, 40}));
  EXPECT_EQ(first, 0);
  EXPECT_EQ(third, 2);
}

TEST(FixedOrderedSlotPoolTest, ClearKeepsCapacityAndAllowsReuse) {
  aquila::FixedOrderedSlotPool<FixedOrderedSlotPoolTestValue, 4> slots;
  EXPECT_EQ(slots.Initialize(2), 2U);

  const auto first =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 7, .payload = 70});
  const auto second =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 8, .payload = 80});
  ASSERT_NE(first, decltype(slots)::kInvalidIndex);
  ASSERT_NE(second, decltype(slots)::kInvalidIndex);

  slots.Clear();

  EXPECT_TRUE(slots.empty());
  EXPECT_EQ(slots.capacity(), 2U);
  EXPECT_FALSE(slots.occupied(first));
  const auto reused =
      slots.EmplaceBack(FixedOrderedSlotPoolTestValue{.id = 9, .payload = 90});
  EXPECT_EQ(reused, first);
  EXPECT_EQ(slots.At(reused).payload, 90);
}

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

TEST(HistogramQuantileTest, SimdValuesMatchScalarValueForUpperQuantile) {
  aquila::HistogramQuantile<double> quantile;
  quantile.Init(0.0, 1.0, 17, 0.75,
                aquila::HistogramQuantileValueMode::kMidpoint);

  for (const double value :
       {0.01, 0.02, 0.11, 0.12, 0.13, 0.21, 0.31, 0.41, 0.42, 0.51, 0.61, 0.62,
        0.71, 0.81, 0.91, 0.92, 0.93}) {
    quantile.Add(value);
  }

  EXPECT_DOUBLE_EQ(quantile.ValueScalar(), 12.5 / 17.0);
  EXPECT_DOUBLE_EQ(quantile.ValueScalar(), quantile.ValueAvx2());
  EXPECT_DOUBLE_EQ(quantile.ValueScalar(), quantile.ValueAvx512());
}

TEST(HistogramQuantileTest, SimdValuesMatchScalarValueForSparseTouchedBins) {
  aquila::HistogramQuantile<double> quantile;
  quantile.Init(900.0, 1100.0, 10000, 0.6,
                aquila::HistogramQuantileValueMode::kMidpoint);

  for (const double value :
       {980.01, 990.0, 1000.0, 1005.0, 1010.0, 1014.0, 1015.0}) {
    quantile.Add(value);
  }

  EXPECT_NEAR(quantile.ValueScalar(), 1010.01, 1e-12);
  EXPECT_DOUBLE_EQ(quantile.ValueScalar(), quantile.ValueAvx2());
  EXPECT_DOUBLE_EQ(quantile.ValueScalar(), quantile.ValueAvx512());
}

TEST(HistogramQuantileTest, ResetClearsOnlyTouchedBinsWithoutStaleValues) {
  aquila::HistogramQuantile<double> quantile;
  quantile.Init(900.0, 1100.0, 10000, 0.6,
                aquila::HistogramQuantileValueMode::kMidpoint);

  quantile.Add(980.0);
  quantile.Add(1015.0);
  quantile.Reset();
  quantile.Add(900.01);

  EXPECT_EQ(quantile.count(), 1U);
  EXPECT_NEAR(quantile.ValueScalar(), 900.01, 1e-12);
  EXPECT_DOUBLE_EQ(quantile.ValueScalar(), quantile.ValueAvx2());
  EXPECT_DOUBLE_EQ(quantile.ValueScalar(), quantile.ValueAvx512());
}

}  // namespace
