#ifndef AQUILA_CORE_BASE_FIXED_ORDERED_SLOT_POOL_H_
#define AQUILA_CORE_BASE_FIXED_ORDERED_SLOT_POOL_H_

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace aquila {

template <typename T, std::size_t kCapacity,
          typename Index = std::conditional_t<(kCapacity <= 255U), std::uint8_t,
                                              std::uint16_t>>
class FixedOrderedSlotPool {
  static_assert(kCapacity > 0U);
  static_assert(kCapacity <= 64U);
  static_assert(std::is_unsigned_v<Index>);
  static_assert(kCapacity <
                static_cast<std::size_t>(std::numeric_limits<Index>::max()));
  static_assert(std::is_nothrow_default_constructible_v<T>);
  static_assert(std::is_nothrow_move_assignable_v<T>);

 public:
  static constexpr Index kInvalidIndex = std::numeric_limits<Index>::max();

  FixedOrderedSlotPool() noexcept {
    ResetInactiveIndices();
  }

  FixedOrderedSlotPool(const FixedOrderedSlotPool&) = delete;
  FixedOrderedSlotPool& operator=(const FixedOrderedSlotPool&) = delete;
  FixedOrderedSlotPool(FixedOrderedSlotPool&& other) noexcept
      : slots_(std::move(other.slots_)),
        occupied_mask_(other.occupied_mask_),
        capacity_mask_(other.capacity_mask_),
        capacity_(other.capacity_),
        active_count_(other.active_count_),
        active_indices_(other.active_indices_),
        active_positions_(other.active_positions_) {
    other.ResetInactive();
  }

  FixedOrderedSlotPool& operator=(FixedOrderedSlotPool&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    slots_ = std::move(other.slots_);
    occupied_mask_ = other.occupied_mask_;
    capacity_mask_ = other.capacity_mask_;
    capacity_ = other.capacity_;
    active_count_ = other.active_count_;
    active_indices_ = other.active_indices_;
    active_positions_ = other.active_positions_;
    other.ResetInactive();
    return *this;
  }

  [[nodiscard]] std::size_t Initialize(
      std::size_t requested_capacity) noexcept {
    const Index target_capacity = static_cast<Index>(
        requested_capacity < kCapacity ? requested_capacity : kCapacity);
    if (target_capacity == 0) {
      ResetInactive();
      return 0;
    }
    if (slots_ == nullptr || capacity_ != target_capacity) {
      std::unique_ptr<T[]> slots{
          new (std::nothrow) T[static_cast<std::size_t>(target_capacity)]};
      if (slots == nullptr) {
        ResetInactive();
        return 0;
      }
      slots_ = std::move(slots);
    } else {
      for (std::size_t i = 0; i < capacity_; ++i) {
        slots_[i] = T{};
      }
    }
    ResetInactiveIndices();
    occupied_mask_ = 0;
    active_count_ = 0;
    capacity_ = target_capacity;
    capacity_mask_ = BuildCapacityMask(capacity_);
    return capacity_;
  }

  void Clear() noexcept {
    for (std::size_t i = 0; i < kCapacity && i < active_count_; ++i) {
      const Index slot = active_indices_[i];
      slots_[slot] = T{};
      active_indices_[i] = kInvalidIndex;
      active_positions_[slot] = kInvalidIndex;
    }
    occupied_mask_ = 0;
    active_count_ = 0;
  }

  template <typename... Args>
  [[nodiscard]] Index EmplaceBack(Args&&... args) noexcept(
      std::is_nothrow_constructible_v<T, Args...> &&
      std::is_nothrow_move_assignable_v<T>) {
    const std::uint64_t idle_mask = capacity_mask_ & ~occupied_mask_;
    if (idle_mask == 0) {
      return kInvalidIndex;
    }
    const Index slot = static_cast<Index>(std::countr_zero(idle_mask));
    slots_[slot] = T{std::forward<Args>(args)...};
    occupied_mask_ |= MaskFor(slot);
    active_positions_[slot] = active_count_;
    active_indices_[active_count_] = slot;
    ++active_count_;
    return slot;
  }

  [[nodiscard]] bool Erase(Index index) noexcept {
    if (!occupied(index)) {
      return false;
    }
    const Index position = active_positions_[index];
    assert(position < active_count_);
    for (std::size_t i = static_cast<std::size_t>(position) + 1U;
         i < kCapacity && i < active_count_; ++i) {
      const Index moved_slot = active_indices_[i];
      active_indices_[i - 1U] = moved_slot;
      active_positions_[moved_slot] = static_cast<Index>(i - 1U);
    }
    --active_count_;
    active_indices_[active_count_] = kInvalidIndex;
    active_positions_[index] = kInvalidIndex;
    occupied_mask_ &= ~MaskFor(index);
    slots_[index] = T{};
    return true;
  }

  [[nodiscard]] T& At(Index index) noexcept {
    assert(occupied(index));
    return slots_[index];
  }

  [[nodiscard]] const T& At(Index index) const noexcept {
    assert(occupied(index));
    return slots_[index];
  }

  [[nodiscard]] bool occupied(Index index) const noexcept {
    return index < capacity_ && (occupied_mask_ & MaskFor(index)) != 0;
  }

  [[nodiscard]] bool empty() const noexcept {
    return active_count_ == 0;
  }

  [[nodiscard]] bool full() const noexcept {
    return active_count_ == capacity_;
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return capacity_;
  }

  [[nodiscard]] std::size_t active_count() const noexcept {
    return active_count_;
  }

  [[nodiscard]] std::span<const Index> active_indices() const noexcept {
    return std::span<const Index>{active_indices_.data(), active_count_};
  }

  template <typename Pred>
  [[nodiscard]] Index FindIndexIf(Pred&& pred) noexcept(
      noexcept(pred(std::declval<T&>()))) {
    for (std::size_t i = 0; i < kCapacity && i < active_count_; ++i) {
      const Index slot = active_indices_[i];
      if (pred(slots_[slot])) {
        return slot;
      }
    }
    return kInvalidIndex;
  }

  template <typename Pred>
  [[nodiscard]] Index FindIndexIf(Pred&& pred) const
      noexcept(noexcept(pred(std::declval<const T&>()))) {
    for (std::size_t i = 0; i < kCapacity && i < active_count_; ++i) {
      const Index slot = active_indices_[i];
      if (pred(slots_[slot])) {
        return slot;
      }
    }
    return kInvalidIndex;
  }

 private:
  [[nodiscard]] static constexpr std::uint64_t BuildCapacityMask(
      Index capacity) noexcept {
    if (capacity == 0) {
      return 0;
    }
    if (capacity == 64) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    return (1ULL << capacity) - 1ULL;
  }

  [[nodiscard]] static constexpr std::uint64_t MaskFor(Index index) noexcept {
    return 1ULL << index;
  }

  void ResetInactiveIndices() noexcept {
    active_indices_.fill(kInvalidIndex);
    active_positions_.fill(kInvalidIndex);
  }

  void ResetInactive() noexcept {
    slots_.reset();
    occupied_mask_ = 0;
    capacity_mask_ = 0;
    capacity_ = 0;
    active_count_ = 0;
    ResetInactiveIndices();
  }

  std::unique_ptr<T[]> slots_;
  std::uint64_t occupied_mask_{0};
  std::uint64_t capacity_mask_{0};
  Index capacity_{0};
  Index active_count_{0};
  std::array<Index, kCapacity> active_indices_{};
  std::array<Index, kCapacity> active_positions_{};
};

}  // namespace aquila

#endif  // AQUILA_CORE_BASE_FIXED_ORDERED_SLOT_POOL_H_
