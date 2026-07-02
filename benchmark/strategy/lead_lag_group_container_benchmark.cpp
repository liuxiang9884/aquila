#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>

#include <benchmark/benchmark.h>

#include "strategy/lead_lag/execution_state.h"

namespace {

struct ThreadAllocationCounter {
  bool enabled{false};
  std::uint64_t allocations{0};
  std::uint64_t bytes{0};
};

thread_local ThreadAllocationCounter g_thread_allocation_counter;

void CountAllocation(std::size_t size) noexcept {
  if (g_thread_allocation_counter.enabled) {
    ++g_thread_allocation_counter.allocations;
    g_thread_allocation_counter.bytes += static_cast<std::uint64_t>(size);
  }
}

[[nodiscard]] void* AllocateOrThrow(std::size_t size) {
  CountAllocation(size);
  void* const ptr = std::malloc(size == 0 ? 1 : size);
  if (ptr == nullptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

[[nodiscard]] void* AllocateNoThrow(std::size_t size) noexcept {
  CountAllocation(size);
  return std::malloc(size == 0 ? 1 : size);
}

[[nodiscard]] void* AllocateAlignedOrThrow(std::size_t size,
                                           std::size_t alignment) {
  CountAllocation(size);
  void* ptr = nullptr;
  if (::posix_memalign(&ptr, alignment, size == 0 ? 1 : size) != 0) {
    throw std::bad_alloc();
  }
  return ptr;
}

[[nodiscard]] void* AllocateAlignedNoThrow(std::size_t size,
                                           std::size_t alignment) noexcept {
  CountAllocation(size);
  void* ptr = nullptr;
  if (::posix_memalign(&ptr, alignment, size == 0 ? 1 : size) != 0) {
    return nullptr;
  }
  return ptr;
}

}  // namespace

void* operator new(std::size_t size) {
  return AllocateOrThrow(size);
}

void* operator new[](std::size_t size) {
  return AllocateOrThrow(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return AllocateNoThrow(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return AllocateNoThrow(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateAlignedOrThrow(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateAlignedOrThrow(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  return AllocateAlignedNoThrow(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  return AllocateAlignedNoThrow(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, std::align_val_t,
                     const std::nothrow_t&) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr, std::align_val_t,
                       const std::nothrow_t&) noexcept {
  std::free(ptr);
}

namespace aquila::strategy::leadlag {
namespace {

inline constexpr std::uint16_t kInvalidIndex =
    std::numeric_limits<std::uint16_t>::max();
inline constexpr double kScanPrice = 101.0;
inline constexpr double kTrailingUpdatePrice = 101.5;
inline constexpr double kTrailingStopRate = 0.01;
inline constexpr double kLongNoTriggerTrailingPrice = 100.0;
inline constexpr double kShortNoTriggerTrailingPrice = 102.0;
inline constexpr double kLongTriggerTrailingPrice = 104.0;
inline constexpr double kShortTriggerTrailingPrice = 98.0;
inline constexpr std::uint64_t kChecksumOffset = 1469598103934665603ULL;
inline constexpr std::uint64_t kChecksumPrime = 1099511628211ULL;

enum class ScanScenario : std::uint8_t {
  kNoTrigger,
  kTriggerFirst,
  kTriggerMiddle,
  kTriggerLast,
};

struct AllocationSample {
  std::uint64_t allocations{0};
  std::uint64_t bytes{0};
};

struct AllocationTotals {
  std::uint64_t allocations{0};
  std::uint64_t bytes{0};

  void Add(AllocationSample sample) noexcept {
    allocations += sample.allocations;
    bytes += sample.bytes;
  }
};

void BeginAllocationCounting() noexcept {
  g_thread_allocation_counter.allocations = 0;
  g_thread_allocation_counter.bytes = 0;
  g_thread_allocation_counter.enabled = true;
}

[[nodiscard]] AllocationSample EndAllocationCounting() noexcept {
  g_thread_allocation_counter.enabled = false;
  return AllocationSample{
      .allocations = g_thread_allocation_counter.allocations,
      .bytes = g_thread_allocation_counter.bytes};
}

void SetAllocationCounters(benchmark::State& state,
                           const AllocationTotals& totals) {
  state.counters["measured_allocs"] =
      benchmark::Counter(static_cast<double>(totals.allocations),
                         benchmark::Counter::kAvgIterations);
  state.counters["measured_bytes"] = benchmark::Counter(
      static_cast<double>(totals.bytes), benchmark::Counter::kAvgIterations);
}

[[nodiscard]] std::uint64_t MixChecksum(std::uint64_t checksum,
                                        std::uint64_t value) noexcept {
  checksum ^= value;
  checksum *= kChecksumPrime;
  return checksum;
}

[[nodiscard]] bool LongSlot(std::uint16_t slot) noexcept {
  return slot % 2U == 0U;
}

[[nodiscard]] ExecutionGroup MakeHoldGroup(std::uint64_t group_id,
                                           std::uint16_t slot) noexcept {
  const bool long_position = LongSlot(slot);
  const double quantity =
      (long_position ? 1.0 : -1.0) * (1.0 + 0.125 * static_cast<double>(slot));
  return ExecutionGroup{
      .stage = ExecutionStage::kHold,
      .signed_position_quantity = quantity,
      .absolute_entry_value = 1'000.0 + 10.0 * static_cast<double>(slot),
      .trailing_price = long_position ? kLongNoTriggerTrailingPrice
                                      : kShortNoTriggerTrailingPrice,
      .group_id = group_id,
      .pending_order_count = 0,
      .unknown_result_pending_count = 0,
      .normal_close_retry_count = 0,
      .pending_open_order_count = 0,
      .pending_close_order_count = 0,
  };
}

void MakeGroupTrigger(ExecutionGroup& group) noexcept {
  group.trailing_price = group.long_position() ? kLongTriggerTrailingPrice
                                               : kShortTriggerTrailingPrice;
}

[[nodiscard]] bool ShouldTriggerStoploss(const ExecutionGroup& group,
                                         double price) noexcept {
  if (!group.hold() || !group.can_submit_exit()) {
    return false;
  }
  if (group.long_position()) {
    return price <= group.trailing_price * (1.0 - kTrailingStopRate);
  }
  if (group.short_position()) {
    return price >= group.trailing_price * (1.0 + kTrailingStopRate);
  }
  return false;
}

template <std::uint16_t kCapacity>
class ActiveIndexGroupContainer {
 public:
  static constexpr std::uint16_t kCapacityValue = kCapacity;

  void Reset(std::uint16_t active_count,
             ScanScenario scenario = ScanScenario::kNoTrigger) noexcept {
    groups_.fill(ExecutionGroup{});
    active_indices_.fill(kInvalidIndex);
    active_group_count_ = 0;
    next_group_id_ = 1;
    for (std::uint16_t i = 0; i < active_count; ++i) {
      if (!InsertHoldGroup()) {
        break;
      }
    }
    ApplyScenario(scenario);
  }

  [[nodiscard]] std::uint16_t active_group_count() const noexcept {
    return active_group_count_;
  }

  template <typename Fn>
  void ForEachActive(Fn&& fn) const noexcept {
    for (std::uint16_t i = 0; i < active_group_count_; ++i) {
      const std::uint16_t index = active_indices_[i];
      if (!fn(index, groups_[index])) {
        break;
      }
    }
  }

  template <typename Fn>
  void ForEachActiveMutable(Fn&& fn) noexcept {
    for (std::uint16_t i = 0; i < active_group_count_; ++i) {
      const std::uint16_t index = active_indices_[i];
      if (!fn(index, groups_[index])) {
        break;
      }
    }
  }

  [[nodiscard]] std::uint64_t ClearHeadThenInsert() noexcept {
    return ClearActiveOffsetAndInsert(0);
  }

  [[nodiscard]] std::uint64_t ClearMiddleThenInsert() noexcept {
    return ClearActiveOffsetAndInsert(active_group_count_ / 2U);
  }

  [[nodiscard]] std::uint64_t ClearTailThenInsert() noexcept {
    return ClearActiveOffsetAndInsert(active_group_count_ - 1U);
  }

 private:
  [[nodiscard]] std::uint16_t FindIdleSlot() const noexcept {
    for (std::uint16_t i = 0; i < kCapacity; ++i) {
      if (!groups_[i].active()) {
        return i;
      }
    }
    return kInvalidIndex;
  }

  [[nodiscard]] bool InsertHoldGroup() noexcept {
    if (active_group_count_ >= kCapacity) {
      return false;
    }
    const std::uint16_t slot = FindIdleSlot();
    if (slot == kInvalidIndex) {
      return false;
    }
    groups_[slot] = MakeHoldGroup(next_group_id_++, slot);
    active_indices_[active_group_count_++] = slot;
    return true;
  }

  [[nodiscard]] std::uint16_t IndexAtActiveOffset(
      std::uint16_t offset) const noexcept {
    if (offset >= active_group_count_) {
      return kInvalidIndex;
    }
    return active_indices_[offset];
  }

  void ClearActiveOffset(std::uint16_t offset) noexcept {
    const std::uint16_t index = IndexAtActiveOffset(offset);
    if (index == kInvalidIndex) {
      return;
    }
    groups_[index] = ExecutionGroup{};
    for (std::uint16_t i = offset; i + 1U < active_group_count_; ++i) {
      active_indices_[i] = active_indices_[i + 1U];
    }
    --active_group_count_;
    active_indices_[active_group_count_] = kInvalidIndex;
  }

  [[nodiscard]] std::uint64_t ClearActiveOffsetAndInsert(
      std::uint16_t offset) noexcept {
    if (active_group_count_ == 0 || offset >= active_group_count_) {
      return 0;
    }
    const std::uint16_t removed_index = IndexAtActiveOffset(offset);
    const std::uint64_t removed_group_id = groups_[removed_index].group_id;
    ClearActiveOffset(offset);
    const bool inserted = InsertHoldGroup();
    std::uint64_t checksum = MixChecksum(kChecksumOffset, removed_group_id);
    checksum = MixChecksum(checksum, inserted ? next_group_id_ : 0U);
    checksum = MixChecksum(checksum, active_group_count_);
    return checksum;
  }

  void ApplyScenario(ScanScenario scenario) noexcept {
    if (scenario == ScanScenario::kNoTrigger || active_group_count_ == 0) {
      return;
    }
    std::uint16_t offset = 0;
    switch (scenario) {
      case ScanScenario::kNoTrigger:
        return;
      case ScanScenario::kTriggerFirst:
        offset = 0;
        break;
      case ScanScenario::kTriggerMiddle:
        offset = active_group_count_ / 2U;
        break;
      case ScanScenario::kTriggerLast:
        offset = active_group_count_ - 1U;
        break;
    }
    MakeGroupTrigger(groups_[IndexAtActiveOffset(offset)]);
  }

  std::array<ExecutionGroup, kCapacity> groups_{};
  std::array<std::uint16_t, kCapacity> active_indices_{};
  std::uint16_t active_group_count_{0};
  std::uint64_t next_group_id_{1};
};

template <std::uint16_t kCapacity>
class LinkedListGroupContainer {
 public:
  static constexpr std::uint16_t kCapacityValue = kCapacity;

  void Reset(std::uint16_t active_count,
             ScanScenario scenario = ScanScenario::kNoTrigger) noexcept {
    groups_.fill(ExecutionGroup{});
    prev_.fill(kInvalidIndex);
    next_.fill(kInvalidIndex);
    head_ = kInvalidIndex;
    tail_ = kInvalidIndex;
    active_group_count_ = 0;
    next_group_id_ = 1;
    for (std::uint16_t i = 0; i < active_count; ++i) {
      if (!InsertHoldGroup()) {
        break;
      }
    }
    ApplyScenario(scenario);
  }

  [[nodiscard]] std::uint16_t active_group_count() const noexcept {
    return active_group_count_;
  }

  template <typename Fn>
  void ForEachActive(Fn&& fn) const noexcept {
    std::uint16_t index = head_;
    while (index != kInvalidIndex) {
      const std::uint16_t next_index = next_[index];
      if (!fn(index, groups_[index])) {
        break;
      }
      index = next_index;
    }
  }

  template <typename Fn>
  void ForEachActiveMutable(Fn&& fn) noexcept {
    std::uint16_t index = head_;
    while (index != kInvalidIndex) {
      const std::uint16_t next_index = next_[index];
      if (!fn(index, groups_[index])) {
        break;
      }
      index = next_index;
    }
  }

  [[nodiscard]] std::uint64_t ClearHeadThenInsert() noexcept {
    return ClearActiveOffsetAndInsert(0);
  }

  [[nodiscard]] std::uint64_t ClearMiddleThenInsert() noexcept {
    return ClearActiveOffsetAndInsert(active_group_count_ / 2U);
  }

  [[nodiscard]] std::uint64_t ClearTailThenInsert() noexcept {
    return ClearActiveOffsetAndInsert(active_group_count_ - 1U);
  }

 private:
  [[nodiscard]] std::uint16_t FindIdleSlot() const noexcept {
    for (std::uint16_t i = 0; i < kCapacity; ++i) {
      if (!groups_[i].active()) {
        return i;
      }
    }
    return kInvalidIndex;
  }

  [[nodiscard]] bool InsertHoldGroup() noexcept {
    if (active_group_count_ >= kCapacity) {
      return false;
    }
    const std::uint16_t slot = FindIdleSlot();
    if (slot == kInvalidIndex) {
      return false;
    }
    groups_[slot] = MakeHoldGroup(next_group_id_++, slot);
    prev_[slot] = tail_;
    next_[slot] = kInvalidIndex;
    if (tail_ != kInvalidIndex) {
      next_[tail_] = slot;
    } else {
      head_ = slot;
    }
    tail_ = slot;
    ++active_group_count_;
    return true;
  }

  [[nodiscard]] std::uint16_t IndexAtActiveOffset(
      std::uint16_t offset) const noexcept {
    std::uint16_t index = head_;
    for (std::uint16_t i = 0; index != kInvalidIndex && i < offset; ++i) {
      index = next_[index];
    }
    return index;
  }

  void ClearIndex(std::uint16_t index) noexcept {
    if (index == kInvalidIndex || !groups_[index].active()) {
      return;
    }
    const std::uint16_t prev_index = prev_[index];
    const std::uint16_t next_index = next_[index];
    if (prev_index != kInvalidIndex) {
      next_[prev_index] = next_index;
    } else {
      head_ = next_index;
    }
    if (next_index != kInvalidIndex) {
      prev_[next_index] = prev_index;
    } else {
      tail_ = prev_index;
    }
    prev_[index] = kInvalidIndex;
    next_[index] = kInvalidIndex;
    groups_[index] = ExecutionGroup{};
    --active_group_count_;
  }

  [[nodiscard]] std::uint64_t ClearActiveOffsetAndInsert(
      std::uint16_t offset) noexcept {
    if (active_group_count_ == 0 || offset >= active_group_count_) {
      return 0;
    }
    const std::uint16_t removed_index = IndexAtActiveOffset(offset);
    const std::uint64_t removed_group_id = groups_[removed_index].group_id;
    ClearIndex(removed_index);
    const bool inserted = InsertHoldGroup();
    std::uint64_t checksum = MixChecksum(kChecksumOffset, removed_group_id);
    checksum = MixChecksum(checksum, inserted ? next_group_id_ : 0U);
    checksum = MixChecksum(checksum, active_group_count_);
    return checksum;
  }

  void ApplyScenario(ScanScenario scenario) noexcept {
    if (scenario == ScanScenario::kNoTrigger || active_group_count_ == 0) {
      return;
    }
    std::uint16_t offset = 0;
    switch (scenario) {
      case ScanScenario::kNoTrigger:
        return;
      case ScanScenario::kTriggerFirst:
        offset = 0;
        break;
      case ScanScenario::kTriggerMiddle:
        offset = active_group_count_ / 2U;
        break;
      case ScanScenario::kTriggerLast:
        offset = active_group_count_ - 1U;
        break;
    }
    MakeGroupTrigger(groups_[IndexAtActiveOffset(offset)]);
  }

  std::array<ExecutionGroup, kCapacity> groups_{};
  std::array<std::uint16_t, kCapacity> prev_{};
  std::array<std::uint16_t, kCapacity> next_{};
  std::uint16_t head_{kInvalidIndex};
  std::uint16_t tail_{kInvalidIndex};
  std::uint16_t active_group_count_{0};
  std::uint64_t next_group_id_{1};
};

template <typename Container>
[[nodiscard]] std::uint64_t ScanStoploss(Container& container,
                                         double price) noexcept {
  std::uint64_t checksum = kChecksumOffset;
  container.ForEachActive(
      [&](std::uint16_t index, const ExecutionGroup& group) noexcept {
        checksum = MixChecksum(checksum, group.group_id);
        checksum = MixChecksum(checksum, index);
        if (ShouldTriggerStoploss(group, price)) {
          checksum = MixChecksum(checksum, 0xfeed0000ULL | group.group_id);
          return false;
        }
        return true;
      });
  return checksum;
}

template <typename Container>
[[nodiscard]] std::uint64_t LagTickTrailingUpdateNoTrigger(
    Container& container) noexcept {
  std::uint64_t checksum = kChecksumOffset;
  container.ForEachActiveMutable(
      [&](std::uint16_t index, ExecutionGroup& group) noexcept {
        if (group.long_position()) {
          group.trailing_price =
              std::max(group.trailing_price, kTrailingUpdatePrice);
        } else if (group.short_position()) {
          group.trailing_price =
              std::min(group.trailing_price, kTrailingUpdatePrice);
        }
        checksum = MixChecksum(checksum, group.group_id);
        checksum = MixChecksum(checksum, index);
        checksum = MixChecksum(
            checksum, static_cast<std::uint64_t>(group.trailing_price * 1000));
        if (ShouldTriggerStoploss(group, kTrailingUpdatePrice)) {
          checksum = MixChecksum(checksum, 0xbad00000ULL | group.group_id);
          return false;
        }
        return true;
      });
  return checksum;
}

template <typename Container>
[[nodiscard]] std::uint64_t MixedLivePattern(Container& container) noexcept {
  std::uint64_t checksum = kChecksumOffset;
  checksum = MixChecksum(checksum, ScanStoploss(container, kScanPrice));
  checksum = MixChecksum(checksum, ScanStoploss(container, kScanPrice));
  checksum = MixChecksum(checksum, LagTickTrailingUpdateNoTrigger(container));
  checksum = MixChecksum(checksum, container.ClearMiddleThenInsert());
  checksum = MixChecksum(checksum, ScanStoploss(container, kScanPrice));
  checksum = MixChecksum(checksum, LagTickTrailingUpdateNoTrigger(container));
  checksum = MixChecksum(checksum, container.ClearHeadThenInsert());
  checksum = MixChecksum(checksum, ScanStoploss(container, kScanPrice));
  checksum = MixChecksum(checksum, container.ClearTailThenInsert());
  checksum = MixChecksum(checksum, ScanStoploss(container, kScanPrice));
  return checksum;
}

[[nodiscard]] bool ActiveCountValid(benchmark::State& state,
                                    std::uint16_t capacity,
                                    std::uint16_t active_count) {
  if (active_count == 0 || active_count > capacity) {
    state.SkipWithError("invalid active_count");
    return false;
  }
  return true;
}

[[nodiscard]] std::uint16_t ActiveCountFromState(benchmark::State& state) {
  return static_cast<std::uint16_t>(state.range(0));
}

template <typename Container>
void BM_ScanNoTrigger(benchmark::State& state) {
  const std::uint16_t active_count = ActiveCountFromState(state);
  if (!ActiveCountValid(state, Container::kCapacityValue, active_count)) {
    return;
  }
  Container container;
  container.Reset(active_count);
  AllocationTotals totals;

  for (auto _ : state) {
    BeginAllocationCounting();
    std::uint64_t checksum = ScanStoploss(container, kScanPrice);
    totals.Add(EndAllocationCounting());
    benchmark::DoNotOptimize(checksum);
  }

  SetAllocationCounters(state, totals);
}

template <typename Container, ScanScenario kScenario>
void BM_ScanTrigger(benchmark::State& state) {
  const std::uint16_t active_count = ActiveCountFromState(state);
  if (!ActiveCountValid(state, Container::kCapacityValue, active_count)) {
    return;
  }
  Container container;
  container.Reset(active_count, kScenario);
  AllocationTotals totals;

  for (auto _ : state) {
    BeginAllocationCounting();
    std::uint64_t checksum = ScanStoploss(container, kScanPrice);
    totals.Add(EndAllocationCounting());
    benchmark::DoNotOptimize(checksum);
  }

  SetAllocationCounters(state, totals);
}

template <typename Container>
void BM_LagTickTrailingUpdateNoTrigger(benchmark::State& state) {
  const std::uint16_t active_count = ActiveCountFromState(state);
  if (!ActiveCountValid(state, Container::kCapacityValue, active_count)) {
    return;
  }
  Container container;
  container.Reset(active_count);
  AllocationTotals totals;

  for (auto _ : state) {
    BeginAllocationCounting();
    std::uint64_t checksum = LagTickTrailingUpdateNoTrigger(container);
    totals.Add(EndAllocationCounting());
    benchmark::DoNotOptimize(checksum);
    benchmark::ClobberMemory();
  }

  SetAllocationCounters(state, totals);
}

template <typename Container, std::uint64_t (Container::*kOperation)() noexcept>
void BM_ClearThenInsert(benchmark::State& state) {
  const std::uint16_t active_count = ActiveCountFromState(state);
  if (!ActiveCountValid(state, Container::kCapacityValue, active_count)) {
    return;
  }
  Container container;
  container.Reset(active_count);
  AllocationTotals totals;

  for (auto _ : state) {
    BeginAllocationCounting();
    std::uint64_t checksum = (container.*kOperation)();
    totals.Add(EndAllocationCounting());
    benchmark::DoNotOptimize(checksum);
    benchmark::ClobberMemory();
  }

  SetAllocationCounters(state, totals);
}

template <typename Container>
void BM_MixedLivePattern(benchmark::State& state) {
  const std::uint16_t active_count = ActiveCountFromState(state);
  if (!ActiveCountValid(state, Container::kCapacityValue, active_count)) {
    return;
  }
  Container container;
  container.Reset(active_count);
  AllocationTotals totals;

  for (auto _ : state) {
    BeginAllocationCounting();
    std::uint64_t checksum = MixedLivePattern(container);
    totals.Add(EndAllocationCounting());
    benchmark::DoNotOptimize(checksum);
    benchmark::ClobberMemory();
  }

  SetAllocationCounters(state, totals);
}

using BenchmarkFunction = void (*)(benchmark::State&);

void RegisterOneBenchmark(const char* container_name, const char* case_name,
                          const char* capacity_name, BenchmarkFunction function,
                          std::initializer_list<std::int64_t> active_counts) {
  std::string name;
  name.reserve(96);
  name.append(container_name)
      .append("/")
      .append(case_name)
      .append("/capacity:")
      .append(capacity_name);
  benchmark::Benchmark* registered =
      benchmark::RegisterBenchmark(name.c_str(), function)
          ->ArgName("active_count")
          ->Unit(benchmark::kNanosecond);
  for (const std::int64_t active_count : active_counts) {
    registered->Arg(active_count);
  }
}

template <typename Container>
void RegisterContainerBenchmarks(
    const char* container_name, const char* capacity_name,
    std::initializer_list<std::int64_t> active_counts) {
  RegisterOneBenchmark(container_name, "ScanNoTrigger", capacity_name,
                       &BM_ScanNoTrigger<Container>, active_counts);
  RegisterOneBenchmark(container_name, "ScanTriggerFirst", capacity_name,
                       &BM_ScanTrigger<Container, ScanScenario::kTriggerFirst>,
                       active_counts);
  RegisterOneBenchmark(container_name, "ScanTriggerMiddle", capacity_name,
                       &BM_ScanTrigger<Container, ScanScenario::kTriggerMiddle>,
                       active_counts);
  RegisterOneBenchmark(container_name, "ScanTriggerLast", capacity_name,
                       &BM_ScanTrigger<Container, ScanScenario::kTriggerLast>,
                       active_counts);
  RegisterOneBenchmark(
      container_name, "LagTickTrailingUpdateNoTrigger", capacity_name,
      &BM_LagTickTrailingUpdateNoTrigger<Container>, active_counts);
  RegisterOneBenchmark(
      container_name, "ClearHeadThenInsert", capacity_name,
      &BM_ClearThenInsert<Container, &Container::ClearHeadThenInsert>,
      active_counts);
  RegisterOneBenchmark(
      container_name, "ClearMiddleThenInsert", capacity_name,
      &BM_ClearThenInsert<Container, &Container::ClearMiddleThenInsert>,
      active_counts);
  RegisterOneBenchmark(
      container_name, "ClearTailThenInsert", capacity_name,
      &BM_ClearThenInsert<Container, &Container::ClearTailThenInsert>,
      active_counts);
  RegisterOneBenchmark(container_name, "MixedLivePattern", capacity_name,
                       &BM_MixedLivePattern<Container>, active_counts);
}

void RegisterAllBenchmarks() {
  RegisterContainerBenchmarks<ActiveIndexGroupContainer<8>>("ActiveIndex", "8",
                                                            {2, 4, 8});
  RegisterContainerBenchmarks<LinkedListGroupContainer<8>>("LinkedList", "8",
                                                           {2, 4, 8});
  RegisterContainerBenchmarks<ActiveIndexGroupContainer<16>>("ActiveIndex",
                                                             "16", {4, 8, 16});
  RegisterContainerBenchmarks<LinkedListGroupContainer<16>>("LinkedList", "16",
                                                            {4, 8, 16});
  RegisterContainerBenchmarks<ActiveIndexGroupContainer<32>>("ActiveIndex",
                                                             "32", {8, 16, 32});
  RegisterContainerBenchmarks<LinkedListGroupContainer<32>>("LinkedList", "32",
                                                            {8, 16, 32});
}

const bool kBenchmarksRegistered = [] {
  RegisterAllBenchmarks();
  return true;
}();

}  // namespace
}  // namespace aquila::strategy::leadlag
