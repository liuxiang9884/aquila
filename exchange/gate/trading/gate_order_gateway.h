#ifndef AQUILA_EXCHANGE_GATE_TRADING_GATE_ORDER_GATEWAY_H_
#define AQUILA_EXCHANGE_GATE_TRADING_GATE_ORDER_GATEWAY_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "exchange/gate/trading/order_codecs.h"
#include "exchange/gate/trading/order_types.h"
#include "strategy/order_types.h"

namespace aquila::strategy {

struct GateOrderCache {
  std::array<char, 64> contract_buffer{};
  std::array<char, 32> price_buffer{};
  std::array<char, 8> tif_buffer{};
  std::array<char, 32> text_buffer{};
  gate::OrderWireFields wire{};
};

struct GateStrategyOrder : StrategyOrder {
  GateOrderCache gate{};
};

namespace detail {

template <std::size_t N>
[[nodiscard]] std::string_view CopyToOwnedBuffer(
    std::string_view value, std::array<char, N>& buffer) noexcept {
  if (value.empty() || value.size() > buffer.size()) {
    return {};
  }
  std::copy_n(value.data(), value.size(), buffer.data());
  return std::string_view(buffer.data(), value.size());
}

[[nodiscard]] constexpr bool IsLimitOrder(OrderType type) noexcept {
  switch (type) {
    case OrderType::kLimit:
      return true;
    case OrderType::kMarket:
      return false;
  }
  return false;
}

[[nodiscard]] constexpr std::string_view TimeInForceToken(
    TimeInForce time_in_force) noexcept {
  switch (time_in_force) {
    case TimeInForce::kGoodTillCancel:
      return "gtc";
    case TimeInForce::kImmediateOrCancel:
      return "ioc";
  }
  return {};
}

[[nodiscard]] constexpr OrderResponseKind MapOrderResponseKind(
    gate::OrderResponseKind kind) noexcept {
  switch (kind) {
    case gate::OrderResponseKind::kAck:
      return OrderResponseKind::kAck;
    case gate::OrderResponseKind::kAccepted:
      return OrderResponseKind::kAccepted;
    case gate::OrderResponseKind::kRejected:
      return OrderResponseKind::kRejected;
    case gate::OrderResponseKind::kCancelAccepted:
      return OrderResponseKind::kCancelAccepted;
    case gate::OrderResponseKind::kCancelRejected:
      return OrderResponseKind::kCancelRejected;
  }
  return OrderResponseKind::kRejected;
}

}  // namespace detail

template <typename OrderSessionT>
class GateOrderGateway {
 public:
  using Order = GateStrategyOrder;

  explicit GateOrderGateway(OrderSessionT& session) noexcept
      : session_(session) {}

  [[nodiscard]] bool PrepareOrder(Order& order,
                                  const OrderDraft& draft) noexcept {
    order.gate = GateOrderCache{};

    if (order.local_order_id <= 0 || !detail::IsLimitOrder(draft.type)) {
      return false;
    }

    const std::string_view contract =
        detail::CopyToOwnedBuffer(draft.symbol, order.gate.contract_buffer);
    const std::string_view price_text =
        detail::CopyToOwnedBuffer(draft.price_text, order.gate.price_buffer);
    const std::string_view tif_token =
        detail::TimeInForceToken(draft.time_in_force);
    const std::string_view tif =
        detail::CopyToOwnedBuffer(tif_token, order.gate.tif_buffer);
    const std::string_view text = gate::OrderTextCodec::Format(
        order.local_order_id, order.gate.text_buffer);
    if (contract.empty() || price_text.empty() || tif.empty() || text.empty()) {
      order.gate = GateOrderCache{};
      return false;
    }

    order.exchange = draft.exchange;
    order.symbol_id = draft.symbol_id;
    order.side = draft.side;
    order.type = draft.type;
    order.time_in_force = draft.time_in_force;
    order.signed_quantity = draft.signed_quantity;
    order.reduce_only = draft.reduce_only;

    order.gate.wire = gate::OrderWireFields{
        .local_order_id = order.local_order_id,
        .contract = contract,
        .signed_size = draft.signed_quantity,
        .price_text = price_text,
        .tif = tif,
        .text = text,
        .reduce_only = draft.reduce_only,
    };
    return true;
  }

  [[nodiscard]] GatewaySendResult PlaceOrder(Order& order) noexcept {
    if (!IsWireReadyForPlace(order)) {
      return {.status = GatewaySendStatus::kRejected};
    }
    const gate::OrderSendResult result =
        session_.PlaceOrder(gate::PlaceOrderRequest{.wire = order.gate.wire});
    return {.status = MapSendStatus(result.status)};
  }

  [[nodiscard]] GatewaySendResult CancelOrder(Order& order) noexcept {
    const gate::OrderSendResult result = session_.CancelOrder(
        gate::CancelOrderRequest{.local_order_id = order.local_order_id,
                                 .exchange_order_id = order.exchange_order_id});
    return {.status = MapSendStatus(result.status)};
  }

 private:
  [[nodiscard]] static constexpr GatewaySendStatus MapSendStatus(
      gate::OrderSendStatus status) noexcept {
    return status == gate::OrderSendStatus::kOk ? GatewaySendStatus::kOk
                                                : GatewaySendStatus::kRejected;
  }

  [[nodiscard]] static bool IsWireReadyForPlace(const Order& order) noexcept {
    return order.local_order_id > 0 &&
           order.gate.wire.local_order_id == order.local_order_id &&
           !order.gate.wire.contract.empty() &&
           !order.gate.wire.price_text.empty() &&
           !order.gate.wire.tif.empty() && !order.gate.wire.text.empty();
  }

  OrderSessionT& session_;
};

[[nodiscard]] constexpr OrderResponseEvent ToStrategyOrderResponse(
    const gate::OrderResponse& response) noexcept {
  return OrderResponseEvent{
      .kind = detail::MapOrderResponseKind(response.kind),
      .local_order_id = response.local_order_id,
      .exchange_order_id = response.exchange_order_id,
      .error_label_hash = response.error_label_hash,
  };
}

}  // namespace aquila::strategy

#endif  // AQUILA_EXCHANGE_GATE_TRADING_GATE_ORDER_GATEWAY_H_
