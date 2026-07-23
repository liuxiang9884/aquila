#ifndef AQUILA_TOOLS_GATE_STRATEGY_ORDER_RESPONSE_CONVERSION_H_
#define AQUILA_TOOLS_GATE_STRATEGY_ORDER_RESPONSE_CONVERSION_H_

#include "core/trading/order_types.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::tools::gate_strategy_order {

[[nodiscard]] inline core::OrderResponseKind ToCoreKind(
    gate::OrderResponseKind kind) noexcept {
  switch (kind) {
    case gate::OrderResponseKind::kAck:
      return core::OrderResponseKind::kAck;
    case gate::OrderResponseKind::kAccepted:
      return core::OrderResponseKind::kAccepted;
    case gate::OrderResponseKind::kRejected:
      return core::OrderResponseKind::kRejected;
    case gate::OrderResponseKind::kCancelAccepted:
      return core::OrderResponseKind::kCancelAccepted;
    case gate::OrderResponseKind::kCancelRejected:
      return core::OrderResponseKind::kCancelRejected;
  }
  return core::OrderResponseKind::kRejected;
}

[[nodiscard]] inline bool IsUnknownResultResponse(
    const gate::OrderResponse& response) noexcept {
  if (response.http_status < 500) {
    return false;
  }
  return response.kind == gate::OrderResponseKind::kRejected ||
         response.kind == gate::OrderResponseKind::kCancelRejected;
}

[[nodiscard]] inline core::OrderResponseKind ToCoreKind(
    const gate::OrderResponse& response) noexcept {
  if (IsUnknownResultResponse(response)) {
    return core::OrderResponseKind::kUnknownResult;
  }
  return ToCoreKind(response.kind);
}

[[nodiscard]] inline core::OrderResponseEvent ToCoreEvent(
    const gate::OrderResponse& response) noexcept {
  return core::OrderResponseEvent{
      .kind = ToCoreKind(response),
      .local_order_id = response.local_order_id,
      .group_id = response.group_id,
      .exchange_order_id = response.exchange_order_id,
      .route_id = response.route_id,
      .local_receive_ns = response.local_receive_ns,
      .exchange_ns = response.exchange_ns,
  };
}

}  // namespace aquila::tools::gate_strategy_order

#endif  // AQUILA_TOOLS_GATE_STRATEGY_ORDER_RESPONSE_CONVERSION_H_
