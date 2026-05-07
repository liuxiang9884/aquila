#ifndef AQUILA_STRATEGY_ORDER_STORE_H_
#define AQUILA_STRATEGY_ORDER_STORE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace aquila::strategy {

template <class OrderT>
class OrderStore {
 public:
  explicit OrderStore(std::size_t capacity) : capacity_(capacity) {
    orders_.reserve(capacity_);
    local_index_.reserve(capacity_);
    exchange_index_.reserve(capacity_);
  }

  OrderStore(const OrderStore&) = delete;
  OrderStore& operator=(const OrderStore&) = delete;

  OrderT* Create() {
    if (orders_.size() >= capacity_) {
      return nullptr;
    }

    const std::int64_t local_order_id = next_local_order_id_++;
    orders_.emplace_back();
    const std::size_t order_index = orders_.size() - 1;
    OrderT& order = orders_[order_index];
    order.local_order_id = local_order_id;
    local_index_.emplace(local_order_id, order_index);
    return &order;
  }

  OrderT* Find(std::int64_t local_order_id) {
    if (local_order_id <= 0) {
      return nullptr;
    }
    const auto it = local_index_.find(local_order_id);
    if (it == local_index_.end()) {
      return nullptr;
    }
    return &orders_[it->second];
  }

  const OrderT* Find(std::int64_t local_order_id) const {
    if (local_order_id <= 0) {
      return nullptr;
    }
    const auto it = local_index_.find(local_order_id);
    if (it == local_index_.end()) {
      return nullptr;
    }
    return &orders_[it->second];
  }

  bool BindExchangeOrderId(std::int64_t local_order_id,
                           std::uint64_t exchange_order_id) {
    if (local_order_id <= 0 || exchange_order_id == 0) {
      return false;
    }

    OrderT* order = Find(local_order_id);
    if (order == nullptr) {
      return false;
    }

    auto existing = exchange_index_.find(exchange_order_id);
    if (existing != exchange_index_.end()) {
      if (existing->second != local_order_id) {
        return false;
      }
    }

    const std::uint64_t previous_exchange_order_id = order->exchange_order_id;
    if (previous_exchange_order_id != 0 &&
        previous_exchange_order_id != exchange_order_id) {
      exchange_index_.erase(previous_exchange_order_id);
    }

    if (existing != exchange_index_.end()) {
      existing->second = local_order_id;
    } else {
      exchange_index_.emplace(exchange_order_id, local_order_id);
    }
    order->exchange_order_id = exchange_order_id;
    return true;
  }

  OrderT* FindByExchangeOrderId(std::uint64_t exchange_order_id) {
    if (exchange_order_id == 0) {
      return nullptr;
    }
    const auto it = exchange_index_.find(exchange_order_id);
    if (it == exchange_index_.end()) {
      return nullptr;
    }
    return Find(it->second);
  }

  const OrderT* FindByExchangeOrderId(std::uint64_t exchange_order_id) const {
    if (exchange_order_id == 0) {
      return nullptr;
    }
    const auto it = exchange_index_.find(exchange_order_id);
    if (it == exchange_index_.end()) {
      return nullptr;
    }
    return Find(it->second);
  }

  std::size_t size() const {
    return orders_.size();
  }

  std::size_t capacity() const {
    return capacity_;
  }

 private:
  std::size_t capacity_{0};
  std::int64_t next_local_order_id_{1};
  std::vector<OrderT> orders_;
  absl::flat_hash_map<std::int64_t, std::size_t> local_index_;
  absl::flat_hash_map<std::uint64_t, std::int64_t> exchange_index_;
};

}  // namespace aquila::strategy

#endif  // AQUILA_STRATEGY_ORDER_STORE_H_
