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

[[nodiscard]] inline core::OrderResponseEvent ToCoreEvent(
    const gate::OrderResponse& response) noexcept {
  return core::OrderResponseEvent{
      .kind = ToCoreKind(response.kind),
      .local_order_id = response.local_order_id,
      .exchange_order_id = response.exchange_order_id,
      .local_receive_ns = response.local_receive_ns,
      .exchange_ns = response.exchange_ns,
  };
}

}  // namespace aquila::tools::gate_strategy_order

#endif  // AQUILA_TOOLS_GATE_STRATEGY_ORDER_RESPONSE_CONVERSION_H_
