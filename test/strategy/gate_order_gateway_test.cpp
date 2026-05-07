#include "strategy/gate_order_gateway.h"

#include <array>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "exchange/gate/trading/order_types.h"
#include "strategy/order_types.h"

namespace aquila::strategy {
namespace {

struct FakeGateOrderSession {
  gate::PlaceOrderRequest last_place_request;
  gate::CancelOrderRequest last_cancel_request;
  gate::OrderSendStatus place_status{gate::OrderSendStatus::kOk};
  gate::OrderSendStatus cancel_status{gate::OrderSendStatus::kOk};
  int place_calls{0};
  int cancel_calls{0};

  gate::OrderSendResult PlaceOrder(
      const gate::PlaceOrderRequest& request) noexcept {
    ++place_calls;
    last_place_request = request;
    return {.status = place_status, .request_sequence = 2};
  }

  gate::OrderSendResult CancelOrder(
      const gate::CancelOrderRequest& request) noexcept {
    ++cancel_calls;
    last_cancel_request = request;
    return {.status = cancel_status, .request_sequence = 3};
  }
};

OrderDraft MakeGateDraft() noexcept {
  return OrderDraft{.exchange = Exchange::kGate,
                    .symbol_id = 7,
                    .symbol = "BTC_USDT",
                    .side = OrderSide::kBuy,
                    .type = OrderType::kLimit,
                    .time_in_force = TimeInForce::kGoodTillCancel,
                    .signed_quantity = 1,
                    .price_text = "81000",
                    .reduce_only = false};
}

void ExpectGateWireEmpty(const GateStrategyOrder& order) {
  EXPECT_EQ(order.gate.wire.local_order_id, 0);
  EXPECT_TRUE(order.gate.wire.contract.empty());
  EXPECT_EQ(order.gate.wire.signed_size, 0);
  EXPECT_TRUE(order.gate.wire.price_text.empty());
  EXPECT_TRUE(order.gate.wire.tif.empty());
  EXPECT_TRUE(order.gate.wire.text.empty());
  EXPECT_FALSE(order.gate.wire.reduce_only);
}

void ExpectMappedResponse(gate::OrderResponseKind gate_kind,
                          OrderResponseKind strategy_kind) {
  const OrderResponseEvent event = ToStrategyOrderResponse(
      gate::OrderResponse{.kind = gate_kind,
                          .local_order_id = 12345,
                          .exchange_order_id = 36028827892199865U,
                          .request_sequence = 2,
                          .http_status = 400,
                          .error_label_hash = 99});

  EXPECT_EQ(event.kind, strategy_kind);
  EXPECT_EQ(event.local_order_id, 12345);
  EXPECT_EQ(event.exchange_order_id, 36028827892199865U);
  EXPECT_EQ(event.error_label_hash, 99U);
}

TEST(GateOrderGatewayTest, PreparesGateWireFields) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  order.local_order_id = 12345;

  ASSERT_TRUE(gateway.PrepareOrder(order, MakeGateDraft()));

  EXPECT_EQ(order.exchange, Exchange::kGate);
  EXPECT_EQ(order.symbol_id, 7);
  EXPECT_EQ(order.gate.wire.local_order_id, 12345);
  EXPECT_EQ(order.gate.wire.contract, "BTC_USDT");
  EXPECT_EQ(order.gate.wire.signed_size, 1);
  EXPECT_EQ(order.gate.wire.price_text, "81000");
  EXPECT_EQ(order.gate.wire.tif, "gtc");
  EXPECT_EQ(order.gate.wire.text, "t-12345");
  EXPECT_FALSE(order.gate.wire.reduce_only);
}

TEST(GateOrderGatewayTest, PreservesLimitOrderTypeAndMapsImmediateOrCancel) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  order.local_order_id = 12345;
  OrderDraft draft = MakeGateDraft();
  draft.side = OrderSide::kSell;
  draft.type = OrderType::kLimit;
  draft.time_in_force = TimeInForce::kImmediateOrCancel;
  draft.signed_quantity = -2;
  draft.price_text = "81000";
  draft.reduce_only = true;

  ASSERT_TRUE(gateway.PrepareOrder(order, draft));

  EXPECT_EQ(order.side, OrderSide::kSell);
  EXPECT_EQ(order.type, OrderType::kLimit);
  EXPECT_EQ(order.time_in_force, TimeInForce::kImmediateOrCancel);
  EXPECT_EQ(order.gate.wire.signed_size, -2);
  EXPECT_EQ(order.gate.wire.price_text, "81000");
  EXPECT_EQ(order.gate.wire.tif, "ioc");
  EXPECT_TRUE(order.gate.wire.reduce_only);
}

TEST(GateOrderGatewayTest, RejectsMarketOrderAndClearsCachedWire) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  order.local_order_id = 12345;
  ASSERT_TRUE(gateway.PrepareOrder(order, MakeGateDraft()));
  OrderDraft draft = MakeGateDraft();
  draft.type = OrderType::kMarket;
  draft.time_in_force = TimeInForce::kImmediateOrCancel;
  draft.price_text = "0";

  EXPECT_FALSE(gateway.PrepareOrder(order, draft));

  ExpectGateWireEmpty(order);
  EXPECT_EQ(session.place_calls, 0);
  EXPECT_EQ(session.cancel_calls, 0);
}

TEST(GateOrderGatewayTest, RejectsInvalidLocalOrderIdAndDoesNotCallSession) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  order.local_order_id = 0;

  EXPECT_FALSE(gateway.PrepareOrder(order, MakeGateDraft()));

  ExpectGateWireEmpty(order);
  EXPECT_EQ(session.place_calls, 0);
  EXPECT_EQ(session.cancel_calls, 0);
}

TEST(GateOrderGatewayTest, RejectsOversizedSymbolAndClearsCachedWire) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  order.local_order_id = 12345;
  ASSERT_TRUE(gateway.PrepareOrder(order, MakeGateDraft()));

  std::array<char, 65> oversized_symbol{};
  oversized_symbol.fill('X');
  OrderDraft draft = MakeGateDraft();
  draft.symbol =
      std::string_view(oversized_symbol.data(), oversized_symbol.size());

  EXPECT_FALSE(gateway.PrepareOrder(order, draft));

  ExpectGateWireEmpty(order);
  EXPECT_EQ(session.place_calls, 0);
  EXPECT_EQ(session.cancel_calls, 0);
}

TEST(GateOrderGatewayTest, RejectsOversizedPriceAndClearsCachedWire) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  order.local_order_id = 12345;
  ASSERT_TRUE(gateway.PrepareOrder(order, MakeGateDraft()));

  std::array<char, 33> oversized_price{};
  oversized_price.fill('9');
  OrderDraft draft = MakeGateDraft();
  draft.price_text =
      std::string_view(oversized_price.data(), oversized_price.size());

  EXPECT_FALSE(gateway.PrepareOrder(order, draft));

  ExpectGateWireEmpty(order);
  EXPECT_EQ(session.place_calls, 0);
  EXPECT_EQ(session.cancel_calls, 0);
}

TEST(GateOrderGatewayTest, PrepareFailureAfterSuccessClearsWireAndBlocksPlace) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  order.local_order_id = 12345;
  ASSERT_TRUE(gateway.PrepareOrder(order, MakeGateDraft()));
  OrderDraft draft = MakeGateDraft();
  draft.type = OrderType::kMarket;

  EXPECT_FALSE(gateway.PrepareOrder(order, draft));
  const GatewaySendResult placed = gateway.PlaceOrder(order);

  ExpectGateWireEmpty(order);
  EXPECT_EQ(placed.status, GatewaySendStatus::kRejected);
  EXPECT_EQ(session.place_calls, 0);
}

TEST(GateOrderGatewayTest, PlacesAndCancelsUsingGateOrderSession) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  order.local_order_id = 12345;
  order.exchange_order_id = 36028827892199865U;
  ASSERT_TRUE(gateway.PrepareOrder(order, MakeGateDraft()));

  const GatewaySendResult placed = gateway.PlaceOrder(order);
  const GatewaySendResult cancelled = gateway.CancelOrder(order);

  EXPECT_EQ(placed.status, GatewaySendStatus::kOk);
  EXPECT_EQ(cancelled.status, GatewaySendStatus::kOk);
  EXPECT_EQ(session.place_calls, 1);
  EXPECT_EQ(session.cancel_calls, 1);
  EXPECT_EQ(session.last_place_request.wire.local_order_id, 12345);
  EXPECT_EQ(session.last_cancel_request.local_order_id, 12345);
  EXPECT_EQ(session.last_cancel_request.exchange_order_id, 36028827892199865U);
}

TEST(GateOrderGatewayTest, MapsPlaceRejectedSendStatus) {
  FakeGateOrderSession session;
  session.place_status = gate::OrderSendStatus::kNotActive;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  order.local_order_id = 12345;
  ASSERT_TRUE(gateway.PrepareOrder(order, MakeGateDraft()));

  const GatewaySendResult placed = gateway.PlaceOrder(order);

  EXPECT_EQ(placed.status, GatewaySendStatus::kRejected);
  EXPECT_EQ(session.place_calls, 1);
}

TEST(GateOrderGatewayTest, MapsCancelRejectedSendStatus) {
  FakeGateOrderSession session;
  session.cancel_status = gate::OrderSendStatus::kNotLoggedIn;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  order.local_order_id = 12345;
  ASSERT_TRUE(gateway.PrepareOrder(order, MakeGateDraft()));

  const GatewaySendResult cancelled = gateway.CancelOrder(order);

  EXPECT_EQ(cancelled.status, GatewaySendStatus::kRejected);
  EXPECT_EQ(session.cancel_calls, 1);
}

TEST(GateOrderGatewayTest, MapsEveryGateOrderResponseKindToStrategyEvent) {
  ExpectMappedResponse(gate::OrderResponseKind::kAck, OrderResponseKind::kAck);
  ExpectMappedResponse(gate::OrderResponseKind::kAccepted,
                       OrderResponseKind::kAccepted);
  ExpectMappedResponse(gate::OrderResponseKind::kRejected,
                       OrderResponseKind::kRejected);
  ExpectMappedResponse(gate::OrderResponseKind::kCancelAccepted,
                       OrderResponseKind::kCancelAccepted);
  ExpectMappedResponse(gate::OrderResponseKind::kCancelRejected,
                       OrderResponseKind::kCancelRejected);
}

}  // namespace
}  // namespace aquila::strategy
