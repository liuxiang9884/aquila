#ifndef AQUILA_STRATEGY_STRATEGY_H_
#define AQUILA_STRATEGY_STRATEGY_H_

#include <cstddef>
#include <cstdint>

#include "strategy/order_store.h"
#include "strategy/order_types.h"

namespace aquila::strategy {

template <typename GatewayT>
class Strategy {
 public:
  using Order = typename GatewayT::Order;

  Strategy(GatewayT& gateway, std::size_t order_capacity)
      : gateway_(gateway), orders_(order_capacity) {}

  Strategy(const Strategy&) = delete;
  Strategy& operator=(const Strategy&) = delete;

  OrderCreateResult CreateLimitOrder(OrderDraft draft) noexcept {
    if (draft.symbol.empty() || draft.price_text.empty() ||
        draft.signed_quantity == 0) {
      return {.status = OrderCreateStatus::kInvalidOrder,
              .local_order_id = 0};
    }

    Order* order = orders_.Create();
    if (order == nullptr) {
      return {.status = OrderCreateStatus::kStoreFull, .local_order_id = 0};
    }

    draft.type = OrderType::kLimit;
    CopyExchangeNeutralFields(*order, draft);
    order->status = OrderStatus::kCreated;

    if (!gateway_.PrepareOrder(*order, draft)) {
      order->status = OrderStatus::kRejected;
      return {.status = OrderCreateStatus::kGatewayRejected,
              .local_order_id = order->local_order_id};
    }

    return {.status = OrderCreateStatus::kOk,
            .local_order_id = order->local_order_id};
  }

  OrderSubmitResult SubmitOrder(std::int64_t local_order_id) noexcept {
    Order* order = orders_.Find(local_order_id);
    if (order == nullptr) {
      return {.status = OrderSubmitStatus::kOrderNotFound,
              .local_order_id = local_order_id};
    }
    if (order->status != OrderStatus::kCreated) {
      return {.status = OrderSubmitStatus::kInvalidStatus,
              .local_order_id = local_order_id};
    }

    const GatewaySendResult sent = gateway_.PlaceOrder(*order);
    if (sent.status != GatewaySendStatus::kOk) {
      order->status = OrderStatus::kRejected;
      return {.status = OrderSubmitStatus::kGatewayRejected,
              .local_order_id = local_order_id};
    }

    order->status = OrderStatus::kSubmitted;
    return {.status = OrderSubmitStatus::kOk, .local_order_id = local_order_id};
  }

  OrderCancelResult CancelOrder(std::int64_t local_order_id) noexcept {
    Order* order = orders_.Find(local_order_id);
    if (order == nullptr) {
      return {.status = OrderCancelStatus::kOrderNotFound,
              .local_order_id = local_order_id};
    }
    if (!CanSubmitCancel(order->status)) {
      return {.status = OrderCancelStatus::kInvalidStatus,
              .local_order_id = local_order_id};
    }

    const GatewaySendResult sent = gateway_.CancelOrder(*order);
    if (sent.status != GatewaySendStatus::kOk) {
      return {.status = OrderCancelStatus::kGatewayRejected,
              .local_order_id = local_order_id};
    }

    order->status = OrderStatus::kCancelSubmitted;
    return {.status = OrderCancelStatus::kOk, .local_order_id = local_order_id};
  }

  void OnOrderResponse(const OrderResponseEvent& event) noexcept {
    Order* order = orders_.Find(event.local_order_id);
    if (order == nullptr) {
      return;
    }

    switch (event.kind) {
      case OrderResponseKind::kAck:
        OnAck(*order);
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

  // Mutable lookup is intended for same-thread gateway/test integration; Strategy
  // remains the state owner.
  Order* FindOrder(std::int64_t local_order_id) noexcept {
    return orders_.Find(local_order_id);
  }

  const Order* FindOrder(std::int64_t local_order_id) const noexcept {
    return orders_.Find(local_order_id);
  }

  Order* FindOrderByExchangeOrderId(
      std::uint64_t exchange_order_id) noexcept {
    return orders_.FindByExchangeOrderId(exchange_order_id);
  }

  const Order* FindOrderByExchangeOrderId(
      std::uint64_t exchange_order_id) const noexcept {
    return orders_.FindByExchangeOrderId(exchange_order_id);
  }

  std::size_t order_count() const noexcept { return orders_.size(); }

 private:
  static void CopyExchangeNeutralFields(Order& order,
                                        const OrderDraft& draft) noexcept {
    order.exchange = draft.exchange;
    order.symbol_id = draft.symbol_id;
    order.side = draft.side;
    order.type = OrderType::kLimit;
    order.time_in_force = draft.time_in_force;
    order.signed_quantity = draft.signed_quantity;
    order.exchange_order_id = 0;
    order.reduce_only = draft.reduce_only;
    order.error_label_hash = 0;
  }

  static bool CanSubmitCancel(OrderStatus status) noexcept {
    return status == OrderStatus::kSubmitted || status == OrderStatus::kAcked ||
           status == OrderStatus::kAccepted;
  }

  void OnAck(Order& order) noexcept {
    if (order.status == OrderStatus::kSubmitted) {
      order.status = OrderStatus::kAcked;
    }
  }

  void OnAccepted(Order& order, const OrderResponseEvent& event) noexcept {
    if (order.status == OrderStatus::kSubmitted ||
        order.status == OrderStatus::kAcked) {
      if (!BindExchangeOrderId(order, event)) {
        return;
      }
      order.status = OrderStatus::kAccepted;
      return;
    }

    if (order.status == OrderStatus::kCancelSubmitted) {
      BindExchangeOrderIdIfUnbound(order, event);
    }
  }

  void OnRejected(Order& order, const OrderResponseEvent& event) noexcept {
    if (order.status == OrderStatus::kSubmitted ||
        order.status == OrderStatus::kAcked) {
      order.status = OrderStatus::kRejected;
      order.error_label_hash = event.error_label_hash;
    }
  }

  void OnCancelAccepted(Order& order,
                        const OrderResponseEvent& event) noexcept {
    if (order.status != OrderStatus::kCancelSubmitted) {
      return;
    }
    if (!BindExchangeOrderId(order, event)) {
      return;
    }
    order.status = OrderStatus::kCancelAccepted;
  }

  void OnCancelRejected(Order& order,
                        const OrderResponseEvent& event) noexcept {
    if (order.status == OrderStatus::kCancelSubmitted) {
      order.status = OrderStatus::kCancelRejected;
      order.error_label_hash = event.error_label_hash;
    }
  }

  bool BindExchangeOrderId(Order& order,
                           const OrderResponseEvent& event) noexcept {
    if (event.exchange_order_id == 0) {
      return true;
    }
    if (!orders_.BindExchangeOrderId(event.local_order_id,
                                     event.exchange_order_id)) {
      SaveErrorLabelHashIfPresent(order, event);
      return false;
    }
    return true;
  }

  void BindExchangeOrderIdIfUnbound(
      Order& order, const OrderResponseEvent& event) noexcept {
    if (order.exchange_order_id != 0 || event.exchange_order_id == 0) {
      return;
    }
    if (!orders_.BindExchangeOrderId(event.local_order_id,
                                     event.exchange_order_id)) {
      SaveErrorLabelHashIfPresent(order, event);
    }
  }

  static void SaveErrorLabelHashIfPresent(
      Order& order, const OrderResponseEvent& event) noexcept {
    if (event.error_label_hash != 0) {
      order.error_label_hash = event.error_label_hash;
    }
  }

  GatewayT& gateway_;
  OrderStore<Order> orders_;
};

}  // namespace aquila::strategy

#endif  // AQUILA_STRATEGY_STRATEGY_H_
