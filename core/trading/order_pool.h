#ifndef AQUILA_CORE_TRADING_ORDER_POOL_H_
#define AQUILA_CORE_TRADING_ORDER_POOL_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace aquila {

template <typename OrderT>
class OrderPool {
 public:
  explicit OrderPool(std::size_t max_live_orders)
      : max_live_orders_(max_live_orders) {
    if (max_live_orders_ > kMaxLiveOrders) {
      throw std::invalid_argument("OrderPool max_live_orders is too large");
    }
    const std::size_t slot_capacity = max_live_orders_ * 2;
    index_reserve_size_ = IndexReserveSizeFor(max_live_orders_);
    slots_.resize(slot_capacity);
    local_to_slot_.reserve(index_reserve_size_);

    for (std::size_t i = 0; i < slots_.size(); ++i) {
      slots_[i].next_free = i + 1 < slots_.size()
                                ? static_cast<std::uint32_t>(i + 1)
                                : kInvalidSlot;
    }
    free_head_ = slots_.empty() ? kInvalidSlot : 0;
  }

  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;

  OrderT* Create() {
    if (live_size_ >= max_live_orders_ || free_head_ == kInvalidSlot) {
      return nullptr;
    }

    const std::uint32_t slot_index = free_head_;
    Slot& slot = slots_[slot_index];
    free_head_ = slot.next_free;
    slot.next_free = kInvalidSlot;

    slot.order = OrderT{};
    slot.order.local_order_id = next_local_order_id_++;
    local_to_slot_.emplace(slot.order.local_order_id, slot_index);
    ++live_size_;
    return &slot.order;
  }

  bool Erase(std::int64_t local_order_id) {
    auto it = local_to_slot_.find(local_order_id);
    if (it == local_to_slot_.end()) {
      return false;
    }

    const std::uint32_t slot_index = it->second;
    local_to_slot_.erase(it);

    Slot& slot = slots_[slot_index];
    slot.order = OrderT{};
    slot.next_free = free_head_;
    free_head_ = slot_index;
    --live_size_;
    return true;
  }

  OrderT* Find(std::int64_t local_order_id) {
    auto it = local_to_slot_.find(local_order_id);
    if (it == local_to_slot_.end()) {
      return nullptr;
    }
    return &slots_[it->second].order;
  }

  const OrderT* Find(std::int64_t local_order_id) const {
    auto it = local_to_slot_.find(local_order_id);
    if (it == local_to_slot_.end()) {
      return nullptr;
    }
    return &slots_[it->second].order;
  }

  std::size_t size() const {
    return live_size_;
  }

  std::size_t capacity() const {
    return max_live_orders_;
  }

  std::size_t slot_capacity() const {
    return slots_.size();
  }

  std::size_t index_reserve_size() const {
    return index_reserve_size_;
  }

 private:
  static constexpr std::size_t IndexReserveSizeFor(
      std::size_t max_live_orders) noexcept {
    return max_live_orders * (max_live_orders < 1024 ? 16 : 8);
  }

  static constexpr std::uint32_t kInvalidSlot =
      std::numeric_limits<std::uint32_t>::max();
  static constexpr std::size_t kMaxLiveOrders =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) / 2;

  struct Slot {
    OrderT order{};
    std::uint32_t next_free{kInvalidSlot};
  };

  std::size_t max_live_orders_{0};
  std::size_t live_size_{0};
  std::size_t index_reserve_size_{0};
  std::int64_t next_local_order_id_{1};
  std::uint32_t free_head_{kInvalidSlot};
  std::vector<Slot> slots_;
  absl::flat_hash_map<std::int64_t, std::uint32_t> local_to_slot_;
};

}  // namespace aquila

#endif  // AQUILA_CORE_TRADING_ORDER_POOL_H_
