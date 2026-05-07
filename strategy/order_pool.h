#ifndef AQUILA_STRATEGY_ORDER_POOL_H_
#define AQUILA_STRATEGY_ORDER_POOL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aquila::strategy {

template <class OrderT>
class OrderPool {
 public:
  explicit OrderPool(std::size_t capacity) : capacity_(capacity) {
    orders_.reserve(capacity_);
  }

  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;

  OrderT* Create() {
    if (orders_.size() >= capacity_) {
      return nullptr;
    }

    OrderT& order = orders_.emplace_back();
    order.local_order_id = next_local_order_id_++;
    return &order;
  }

  OrderT* Find(std::int64_t local_order_id) {
    if (local_order_id <= 0) {
      return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(local_order_id - 1);
    if (index >= orders_.size()) {
      return nullptr;
    }
    OrderT& order = orders_[index];
    return order.local_order_id == local_order_id ? &order : nullptr;
  }

  const OrderT* Find(std::int64_t local_order_id) const {
    if (local_order_id <= 0) {
      return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(local_order_id - 1);
    if (index >= orders_.size()) {
      return nullptr;
    }
    const OrderT& order = orders_[index];
    return order.local_order_id == local_order_id ? &order : nullptr;
  }

  bool BindExchangeOrderId(std::int64_t local_order_id,
                           std::uint64_t exchange_order_id) {
    if (exchange_order_id == 0) {
      return false;
    }

    OrderT* order = Find(local_order_id);
    if (order == nullptr) {
      return false;
    }

    for (const OrderT& existing : orders_) {
      if (existing.exchange_order_id == exchange_order_id &&
          existing.local_order_id != local_order_id) {
        return false;
      }
    }

    order->exchange_order_id = exchange_order_id;
    return true;
  }

  OrderT* FindByExchangeOrderId(std::uint64_t exchange_order_id) {
    if (exchange_order_id == 0) {
      return nullptr;
    }
    for (OrderT& order : orders_) {
      if (order.exchange_order_id == exchange_order_id) {
        return &order;
      }
    }
    return nullptr;
  }

  const OrderT* FindByExchangeOrderId(std::uint64_t exchange_order_id) const {
    if (exchange_order_id == 0) {
      return nullptr;
    }
    for (const OrderT& order : orders_) {
      if (order.exchange_order_id == exchange_order_id) {
        return &order;
      }
    }
    return nullptr;
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
};

}  // namespace aquila::strategy

#endif  // AQUILA_STRATEGY_ORDER_POOL_H_
