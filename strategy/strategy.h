#ifndef AQUILA_STRATEGY_STRATEGY_H_
#define AQUILA_STRATEGY_STRATEGY_H_

#include <cstddef>
#include <cstdint>

#include "core/trading/order_pool.h"
#include "strategy/order_types.h"

namespace aquila::strategy {

template <typename GatewayT>
class Strategy {
 public:
  using Order = StrategyOrder;

  Strategy(GatewayT& order_session, std::size_t order_capacity,
           std::uint8_t strategy_id = 0)
      : order_session_(order_session), orders_(order_capacity, strategy_id) {}

  Strategy(const Strategy&) = delete;
  Strategy& operator=(const Strategy&) = delete;

  OrderPlaceResult PlaceLimitOrder(OrderCreateRequest request) noexcept {
    if (request.symbol.empty() || request.price_text.empty() ||
        request.quantity <= 0) {
      return {.status = OrderPlaceStatus::kInvalidOrder, .local_order_id = 0};
    }

    Order* order = CreateOrder(request);
    if (order == nullptr) {
      return {.status = OrderPlaceStatus::kPoolFull, .local_order_id = 0};
    }

    const auto sent = order_session_.PlaceOrder(*order);
    if (!SendOk(sent)) {
      order->status = OrderStatus::kRejected;
      return {.status = OrderPlaceStatus::kSessionRejected,
              .local_order_id = order->local_order_id};
    }

    order->status = OrderStatus::kSent;
    return {.status = OrderPlaceStatus::kOk,
            .local_order_id = order->local_order_id};
  }

  OrderCancelResult CancelOrder(std::uint64_t local_order_id) noexcept {
    Order* order = orders_.Find(local_order_id);
    if (order == nullptr) {
      return {.status = OrderCancelStatus::kOrderNotFound,
              .local_order_id = local_order_id};
    }
    if (!CanSubmitCancel(order->status)) {
      return {.status = OrderCancelStatus::kInvalidStatus,
              .local_order_id = local_order_id};
    }

    const auto sent = order_session_.CancelOrder(*order);
    if (!SendOk(sent)) {
      return {.status = OrderCancelStatus::kSessionRejected,
              .local_order_id = local_order_id};
    }

    order->status = OrderStatus::kCancelSent;
    return {.status = OrderCancelStatus::kOk, .local_order_id = local_order_id};
  }

  void OnOrderResponse(const OrderResponseEvent& event) noexcept {
    Order* order = orders_.Find(event.local_order_id);
    if (order == nullptr) {
      return;
    }

    switch (event.kind) {
      case OrderResponseKind::kAck:
        break;
      case OrderResponseKind::kAccepted:
        OnAccepted(*order, event);
        break;
      case OrderResponseKind::kRejected:
        OnRejected(*order, event);
        break;
      case OrderResponseKind::kCancelAccepted:
        OnCancelAccepted(*order, event);
        break;
      case OrderResponseKind::kCancelRejected:
        OnCancelRejected(*order, event);
        break;
    }
  }

  // Mutable lookup is intended for same-thread gateway/test integration;
  // Strategy remains the state owner.
  Order* FindOrder(std::uint64_t local_order_id) noexcept {
    return orders_.Find(local_order_id);
  }

  const Order* FindOrder(std::uint64_t local_order_id) const noexcept {
    return orders_.Find(local_order_id);
  }

  std::size_t order_count() const noexcept {
    return orders_.size();
  }

 private:
  Order* CreateOrder(const OrderCreateRequest& request) noexcept {
    Order* order = orders_.Create();
    if (order == nullptr) {
      return nullptr;
    }
    order->exchange = request.exchange;
    order->symbol_id = request.symbol_id;
    order->symbol = request.symbol;
    order->side = request.side;
    order->type = OrderType::kLimit;
    order->time_in_force = request.time_in_force;
    order->quantity = request.quantity;
    order->price_text = request.price_text;
    order->reduce_only = request.reduce_only;
    order->status = OrderStatus::kCreated;
    order->error_label_hash = 0;
    return order;
  }

  template <typename SendResultT>
  [[nodiscard]] static bool SendOk(const SendResultT& result) noexcept {
    return result.status == decltype(result.status)::kOk;
  }

  static bool CanSubmitCancel(OrderStatus status) noexcept {
    return status == OrderStatus::kSent || status == OrderStatus::kAccepted ||
           status == OrderStatus::kPartialFilled;
  }

  void OnAccepted(Order& order, const OrderResponseEvent&) noexcept {
    if (order.status == OrderStatus::kSent) {
      order.status = OrderStatus::kAccepted;
      return;
    }
  }

  void OnRejected(Order& order, const OrderResponseEvent& event) noexcept {
    if (order.status == OrderStatus::kSent) {
      order.status = OrderStatus::kRejected;
      order.error_label_hash = event.error_label_hash;
    }
  }

  void OnCancelAccepted(Order& order, const OrderResponseEvent&) noexcept {
    if (order.status != OrderStatus::kCancelSent) {
      return;
    }
    order.status = OrderStatus::kCancelled;
  }

  void OnCancelRejected(Order& order,
                        const OrderResponseEvent& event) noexcept {
    if (order.status == OrderStatus::kCancelSent) {
      order.status = OrderStatus::kRejected;
      order.error_label_hash = event.error_label_hash;
    }
  }

  GatewayT& order_session_;
  OrderPool<Order> orders_;
};

}  // namespace aquila::strategy

#endif  // AQUILA_STRATEGY_STRATEGY_H_
