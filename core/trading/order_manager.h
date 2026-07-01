#ifndef AQUILA_CORE_TRADING_ORDER_MANAGER_H_
#define AQUILA_CORE_TRADING_ORDER_MANAGER_H_

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core/trading/order_feedback_event.h"
#include "core/trading/order_pool.h"
#include "core/trading/order_types.h"

namespace aquila::core {

template <typename GatewayT>
class OrderManager {
 public:
  using Order = StrategyOrder;

  OrderManager(GatewayT& order_session, std::size_t order_capacity,
               std::uint8_t strategy_id = 0)
      : order_session_(order_session), orders_(order_capacity, strategy_id) {}

  OrderManager(const OrderManager&) = delete;
  OrderManager& operator=(const OrderManager&) = delete;

  OrderPlaceResult PlaceOrder(const OrderCreateRequest& request) noexcept {
    if (request.symbol.empty() || request.price_text.empty() ||
        request.quantity_text.empty() || !std::isfinite(request.quantity) ||
        request.quantity <= 0.0) {
      return {.status = OrderPlaceStatus::kInvalidOrder, .local_order_id = 0};
    }

    Order* order = CreateOrder(request);
    if (order == nullptr) {
      return {.status = OrderPlaceStatus::kPoolFull, .local_order_id = 0};
    }

    const auto sent = order_session_.PlaceOrder(*order);
    if (!SendOk(sent)) {
      order->status = OrderStatus::kRejected;
      order->is_finished = true;
      return {.status = OrderPlaceStatus::kSessionRejected,
              .local_order_id = order->local_order_id};
    }

    order->status = OrderStatus::kSent;
    StoreSendLocalNs(*order, sent);
    return {.status = OrderPlaceStatus::kOk,
            .local_order_id = order->local_order_id};
  }

  OrderPlaceResult PlaceLimitOrder(OrderCreateRequest request) noexcept {
    request.order_type = OrderType::kLimit;
    return PlaceOrder(request);
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

    order->pre_cancel_status = order->status;
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
        OnAck(*order, event);
        break;
      case OrderResponseKind::kAccepted:
        OnAccepted(*order, event);
        break;
      case OrderResponseKind::kRejected:
        OnRejected(*order, event);
        break;
      case OrderResponseKind::kUnknownResult:
        OnUnknownResult(*order, event);
        break;
      case OrderResponseKind::kCancelAccepted:
        OnCancelAccepted(*order, event);
        break;
      case OrderResponseKind::kCancelRejected:
        OnCancelRejected(*order);
        break;
    }
  }

  void OnOrderFeedback(const OrderFeedbackEvent& event) noexcept {
    if (event.kind == OrderFeedbackKind::kContinuityLost) {
      OnFeedbackContinuityLost(event);
      return;
    }

    Order* order = orders_.Find(event.local_order_id);
    if (order == nullptr) {
      ++feedback_stats_.unknown_local_order_feedbacks;
      return;
    }
    if (order->is_finished) {
      ++feedback_stats_.terminal_feedbacks_ignored;
      return;
    }

    switch (event.kind) {
      case OrderFeedbackKind::kAccepted:
        OnAcceptedFeedback(*order, event);
        break;
      case OrderFeedbackKind::kPartialFilled:
        OnPartialFilledFeedback(*order, event);
        break;
      case OrderFeedbackKind::kFilled:
        OnFilledFeedback(*order, event);
        break;
      case OrderFeedbackKind::kCancelled:
        OnCancelledFeedback(*order, event);
        break;
      case OrderFeedbackKind::kRejected:
        OnRejectedFeedback(*order, event);
        break;
      case OrderFeedbackKind::kContinuityLost:
        break;
    }
  }

  void OnFeedbackContinuityLost(const OrderFeedbackEvent&) noexcept {
    feedback_continuity_lost_detected_ = true;
    ++feedback_stats_.feedback_continuity_lost_events;
  }

  // Mutable lookup is intended for same-thread gateway/test integration;
  // OrderManager remains the state owner.
  Order* FindOrder(std::uint64_t local_order_id) noexcept {
    return orders_.Find(local_order_id);
  }

  const Order* FindOrder(std::uint64_t local_order_id) const noexcept {
    return orders_.Find(local_order_id);
  }

  bool RetireFinishedOrder(std::uint64_t local_order_id) noexcept {
    const Order* order = orders_.Find(local_order_id);
    if (order == nullptr || !order->is_finished) {
      return false;
    }
    return orders_.Erase(local_order_id);
  }

  std::size_t order_count() const noexcept {
    return orders_.size();
  }

  [[nodiscard]] bool feedback_continuity_lost_detected() const noexcept {
    return feedback_continuity_lost_detected_;
  }

  [[nodiscard]] const StrategyFeedbackStats& feedback_stats() const noexcept {
    return feedback_stats_;
  }

  [[nodiscard]] std::uint16_t MaxOrderSessionFanout() const noexcept {
    if constexpr (requires(const GatewayT& gateway) {
                    gateway.MaxOrderSessionFanout();
                  }) {
      return order_session_.MaxOrderSessionFanout();
    } else if constexpr (requires(const GatewayT& gateway) {
                           gateway.route_count();
                         }) {
      return order_session_.route_count();
    } else {
      return 1;
    }
  }

  [[nodiscard]] bool OrderRouteReady(std::uint16_t route_id) const noexcept {
    if constexpr (requires(const GatewayT& gateway, std::uint16_t route) {
                    gateway.RouteReady(route);
                  }) {
      return order_session_.RouteReady(route_id);
    } else {
      return route_id == 0;
    }
  }

 private:
  static constexpr double kQuantityEpsilon = 1e-12;

  enum class FillApplyResult : std::uint8_t {
    kApplied,
    kDuplicate,
    kStale,
  };

  Order* CreateOrder(const OrderCreateRequest& request) noexcept {
    Order* order = orders_.Create();
    if (order == nullptr) {
      return nullptr;
    }
    order->parent_id = request.parent_id;
    order->exchange = request.exchange;
    order->symbol_id = request.symbol_id;
    order->symbol = request.symbol;
    order->side = request.side;
    order->type = request.order_type;
    order->time_in_force = request.time_in_force;
    order->quantity = request.quantity;
    order->quantity_text = request.quantity_text;
    order->price_text = request.price_text;
    order->reduce_only = request.reduce_only;
    order->gateway_route_id = request.gateway_route_id;
    order->status = OrderStatus::kCreated;
    return order;
  }

  template <typename SendResultT>
  [[nodiscard]] static bool SendOk(const SendResultT& result) noexcept {
    return result.status == decltype(result.status)::kOk;
  }

  template <typename SendResultT>
  static void StoreSendLocalNs(Order& order,
                               const SendResultT& result) noexcept {
    if constexpr (requires { result.send_local_ns; }) {
      order.request_send_local_ns = result.send_local_ns;
    }
  }

  static bool CanSubmitCancel(OrderStatus status) noexcept {
    return status == OrderStatus::kSent || status == OrderStatus::kAccepted ||
           status == OrderStatus::kPartialFilled;
  }

  static void RecordAckTiming(Order& order,
                              const OrderResponseEvent& event) noexcept {
    if (order.ack_local_receive_ns != 0 || order.ack_exchange_ns != 0) {
      return;
    }
    order.ack_local_receive_ns = event.local_receive_ns;
    order.ack_exchange_ns = event.exchange_ns;
  }

  static void RecordResponseTiming(Order& order,
                                   const OrderResponseEvent& event) noexcept {
    if (order.response_local_receive_ns != 0 ||
        order.response_exchange_ns != 0) {
      return;
    }
    order.response_local_receive_ns = event.local_receive_ns;
    order.response_exchange_ns = event.exchange_ns;
  }

  void OnAck(Order& order, const OrderResponseEvent& event) noexcept {
    if (order.status == OrderStatus::kSent) {
      RecordAckTiming(order, event);
    }
  }

  void OnAccepted(Order& order, const OrderResponseEvent& event) noexcept {
    RecordResponseTiming(order, event);
    if (order.status == OrderStatus::kSent) {
      order.status = OrderStatus::kAccepted;
    }
  }

  void OnRejected(Order& order, const OrderResponseEvent& event) noexcept {
    RecordResponseTiming(order, event);
    if (order.status == OrderStatus::kSent) {
      order.status = OrderStatus::kRejected;
      order.is_finished = true;
    }
  }

  void OnUnknownResult(Order& order, const OrderResponseEvent& event) noexcept {
    RecordResponseTiming(order, event);
  }

  void OnCancelAccepted(Order& order, const OrderResponseEvent&) noexcept {
    if (order.status != OrderStatus::kCancelSent) {
      return;
    }
    order.status = OrderStatus::kCancelled;
    order.pre_cancel_status = OrderStatus::kCreated;
  }

  void OnCancelRejected(Order& order) noexcept {
    if (order.status == OrderStatus::kCancelSent) {
      order.status = CanSubmitCancel(order.pre_cancel_status)
                         ? order.pre_cancel_status
                         : OrderStatus::kAccepted;
      order.pre_cancel_status = OrderStatus::kCreated;
    }
  }

  void OnAcceptedFeedback(Order& order,
                          const OrderFeedbackEvent& event) noexcept {
    if (order.status == OrderStatus::kSent) {
      order.status = OrderStatus::kAccepted;
    }
    RecordFeedbackExchangeOrderId(order, event);
    order.exchange_update_ns = event.exchange_update_ns;
    order.accepted_exchange_ns = event.exchange_update_ns;
    NotifyCacheExchangeOrderId(order.local_order_id, event.exchange_order_id);
  }

  void OnPartialFilledFeedback(Order& order,
                               const OrderFeedbackEvent& event) noexcept {
    RecordFeedbackExchangeOrderId(order, event);
    const FillApplyResult result = ApplyCumulativeFill(
        order, event.cumulative_filled_quantity, event.fill_price);
    if (result != FillApplyResult::kApplied) {
      ++feedback_stats_.duplicate_or_stale_feedbacks;
      return;
    }
    if (order.status != OrderStatus::kCancelSent) {
      order.status = OrderStatus::kPartialFilled;
    }
    order.exchange_update_ns = event.exchange_update_ns;
  }

  void OnFilledFeedback(Order& order,
                        const OrderFeedbackEvent& event) noexcept {
    RecordFeedbackExchangeOrderId(order, event);
    const FillApplyResult result = ApplyCumulativeFill(
        order, event.cumulative_filled_quantity, event.fill_price);
    if (result == FillApplyResult::kStale) {
      ++feedback_stats_.duplicate_or_stale_feedbacks;
      return;
    }
    order.status = OrderStatus::kFilled;
    order.role = event.role;
    order.exchange_update_ns = event.exchange_update_ns;
    order.finish_exchange_ns = event.exchange_update_ns;
    FinishOrder(order);
  }

  void OnCancelledFeedback(Order& order,
                           const OrderFeedbackEvent& event) noexcept {
    RecordFeedbackExchangeOrderId(order, event);
    const FillApplyResult result = ApplyCumulativeFill(
        order, event.cumulative_filled_quantity, event.fill_price);
    if (result == FillApplyResult::kStale) {
      ++feedback_stats_.duplicate_or_stale_feedbacks;
      return;
    }
    order.status = event.cumulative_filled_quantity > kQuantityEpsilon
                       ? OrderStatus::kPartiallyCancelled
                       : OrderStatus::kCancelled;
    order.finish_reason = event.finish_reason;
    order.exchange_update_ns = event.exchange_update_ns;
    order.finish_exchange_ns = event.exchange_update_ns;
    FinishOrder(order);
  }

  void OnRejectedFeedback(Order& order,
                          const OrderFeedbackEvent& event) noexcept {
    RecordFeedbackExchangeOrderId(order, event);
    order.status = OrderStatus::kRejected;
    order.reject_reason = event.reject_reason;
    order.exchange_update_ns = event.exchange_update_ns;
    order.finish_exchange_ns = event.exchange_update_ns;
    FinishOrder(order);
  }

  static void RecordFeedbackExchangeOrderId(
      Order& order, const OrderFeedbackEvent& event) noexcept {
    if (event.exchange_order_id != 0) {
      order.exchange_order_id = event.exchange_order_id;
    }
  }

  FillApplyResult ApplyCumulativeFill(Order& order, double cumulative_quantity,
                                      double fill_price) noexcept {
    if (cumulative_quantity + kQuantityEpsilon <
        order.cumulative_filled_quantity) {
      return FillApplyResult::kStale;
    }
    if (std::abs(cumulative_quantity - order.cumulative_filled_quantity) <=
        kQuantityEpsilon) {
      return FillApplyResult::kDuplicate;
    }

    order.cumulative_filled_quantity = cumulative_quantity;
    order.cumulative_filled_value = cumulative_quantity * fill_price;
    order.last_fill_price = fill_price;
    return FillApplyResult::kApplied;
  }

  void FinishOrder(Order& order) noexcept {
    order.is_finished = true;
    order.pre_cancel_status = OrderStatus::kCreated;
    NotifyForgetExchangeOrderId(order.local_order_id);
  }

  void NotifyCacheExchangeOrderId(std::uint64_t local_order_id,
                                  std::uint64_t exchange_order_id) noexcept {
    if (exchange_order_id == 0) {
      return;
    }
    if constexpr (requires {
                    order_session_.CacheExchangeOrderId(local_order_id,
                                                        exchange_order_id);
                  }) {
      order_session_.CacheExchangeOrderId(local_order_id, exchange_order_id);
    }
  }

  void NotifyForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    if constexpr (requires {
                    order_session_.ForgetExchangeOrderId(local_order_id);
                  }) {
      order_session_.ForgetExchangeOrderId(local_order_id);
    } else if constexpr (requires {
                           order_session_
                               .forget_exchange_order_id_for_local_order(
                                   local_order_id);
                         }) {
      (void)order_session_.forget_exchange_order_id_for_local_order(
          local_order_id);
    }
  }

  GatewayT& order_session_;
  OrderPool<Order> orders_;
  StrategyFeedbackStats feedback_stats_{};
  bool feedback_continuity_lost_detected_{false};
};

}  // namespace aquila::core

#endif  // AQUILA_CORE_TRADING_ORDER_MANAGER_H_
