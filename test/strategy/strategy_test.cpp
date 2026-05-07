#include "strategy/strategy.h"

#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "strategy/order_types.h"

namespace aquila::strategy {
namespace {

struct FakeGateway {
  using Order = StrategyOrder;

  bool prepare_ok{true};
  GatewaySendStatus place_status{GatewaySendStatus::kOk};
  GatewaySendStatus cancel_status{GatewaySendStatus::kOk};
  int prepare_calls{0};
  int place_calls{0};
  int cancel_calls{0};
  std::int64_t last_place_local_order_id{0};
  std::int64_t last_cancel_local_order_id{0};

  bool PrepareOrder(Order& order, const OrderDraft& draft) noexcept {
    ++prepare_calls;
    order.exchange = draft.exchange;
    order.symbol_id = draft.symbol_id;
    order.side = draft.side;
    order.type = draft.type;
    order.time_in_force = draft.time_in_force;
    order.signed_quantity = draft.signed_quantity;
    order.reduce_only = draft.reduce_only;
    return prepare_ok;
  }

  GatewaySendResult PlaceOrder(Order& order) noexcept {
    ++place_calls;
    last_place_local_order_id = order.local_order_id;
    return {.status = place_status};
  }

  GatewaySendResult CancelOrder(Order& order) noexcept {
    ++cancel_calls;
    last_cancel_local_order_id = order.local_order_id;
    return {.status = cancel_status};
  }
};

OrderDraft MakeLimitDraft() noexcept {
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

TEST(StrategyTest, CreatesLimitOrderAndKeepsItCreated) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);

  const OrderCreateResult created = strategy.CreateLimitOrder(MakeLimitDraft());

  ASSERT_EQ(created.status, OrderCreateStatus::kOk);
  EXPECT_EQ(created.local_order_id, 1);
  EXPECT_EQ(gateway.prepare_calls, 1);
  const StrategyOrder* order = strategy.FindOrder(created.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCreated);
  EXPECT_EQ(order->exchange, Exchange::kGate);
  EXPECT_EQ(order->symbol_id, 7);
  EXPECT_EQ(order->signed_quantity, 1);
}

TEST(StrategyTest, RejectsInvalidLimitOrderBeforeAllocatingOrder) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  OrderDraft draft = MakeLimitDraft();
  draft.price_text = "";

  const OrderCreateResult created = strategy.CreateLimitOrder(draft);

  EXPECT_EQ(created.status, OrderCreateStatus::kInvalidOrder);
  EXPECT_EQ(created.local_order_id, 0);
  EXPECT_EQ(strategy.order_count(), 0U);
  EXPECT_EQ(gateway.prepare_calls, 0);
}

TEST(StrategyTest, GatewayPrepareRejectKeepsRejectedAttemptHistory) {
  FakeGateway gateway;
  gateway.prepare_ok = false;
  Strategy<FakeGateway> strategy(gateway, 8);

  const OrderCreateResult created = strategy.CreateLimitOrder(MakeLimitDraft());

  EXPECT_EQ(created.status, OrderCreateStatus::kGatewayRejected);
  EXPECT_NE(created.local_order_id, 0);
  EXPECT_EQ(strategy.order_count(), 1U);
  EXPECT_EQ(gateway.prepare_calls, 1);
  const StrategyOrder* order = strategy.FindOrder(created.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kRejected);
}

TEST(StrategyTest, SubmitsCreatedOrderAndRejectsDuplicateSubmit) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderCreateResult created = strategy.CreateLimitOrder(MakeLimitDraft());
  ASSERT_EQ(created.status, OrderCreateStatus::kOk);

  const OrderSubmitResult first = strategy.SubmitOrder(created.local_order_id);
  const OrderSubmitResult second = strategy.SubmitOrder(created.local_order_id);

  EXPECT_EQ(first.status, OrderSubmitStatus::kOk);
  EXPECT_EQ(first.local_order_id, created.local_order_id);
  EXPECT_EQ(second.status, OrderSubmitStatus::kInvalidStatus);
  EXPECT_EQ(gateway.place_calls, 1);
  EXPECT_EQ(gateway.last_place_local_order_id, created.local_order_id);
  const StrategyOrder* order = strategy.FindOrder(created.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kSubmitted);
}

TEST(StrategyTest, GatewayPlaceRejectedMarksOrderRejected) {
  FakeGateway gateway;
  gateway.place_status = GatewaySendStatus::kRejected;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderCreateResult created = strategy.CreateLimitOrder(MakeLimitDraft());
  ASSERT_EQ(created.status, OrderCreateStatus::kOk);

  const OrderSubmitResult submitted =
      strategy.SubmitOrder(created.local_order_id);

  EXPECT_EQ(submitted.status, OrderSubmitStatus::kGatewayRejected);
  EXPECT_EQ(gateway.place_calls, 1);
  const StrategyOrder* order = strategy.FindOrder(created.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kRejected);
}

TEST(StrategyTest, AcceptedResponseStoresExchangeOrderIdIndex) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderCreateResult created = strategy.CreateLimitOrder(MakeLimitDraft());
  ASSERT_EQ(created.status, OrderCreateStatus::kOk);
  ASSERT_EQ(strategy.SubmitOrder(created.local_order_id).status,
            OrderSubmitStatus::kOk);

  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = created.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const StrategyOrder* order = strategy.FindOrder(created.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kAccepted);
  EXPECT_EQ(order->exchange_order_id, 36028827892199865U);
  const StrategyOrder* by_exchange =
      strategy.FindOrderByExchangeOrderId(36028827892199865U);
  ASSERT_NE(by_exchange, nullptr);
  EXPECT_EQ(by_exchange->local_order_id, created.local_order_id);
}

TEST(StrategyTest, DuplicateExchangeOrderIdDoesNotAcceptSecondOrder) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderCreateResult first = strategy.CreateLimitOrder(MakeLimitDraft());
  ASSERT_EQ(first.status, OrderCreateStatus::kOk);
  ASSERT_EQ(strategy.SubmitOrder(first.local_order_id).status,
            OrderSubmitStatus::kOk);
  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = first.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const OrderCreateResult second = strategy.CreateLimitOrder(MakeLimitDraft());
  ASSERT_EQ(second.status, OrderCreateStatus::kOk);
  ASSERT_EQ(strategy.SubmitOrder(second.local_order_id).status,
            OrderSubmitStatus::kOk);
  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = second.local_order_id,
      .exchange_order_id = 36028827892199865U,
      .error_label_hash = 91U,
  });

  const StrategyOrder* second_order = strategy.FindOrder(second.local_order_id);
  ASSERT_NE(second_order, nullptr);
  EXPECT_EQ(second_order->status, OrderStatus::kSubmitted);
  EXPECT_EQ(second_order->exchange_order_id, 0U);
  EXPECT_EQ(second_order->error_label_hash, 91U);
  const StrategyOrder* by_exchange =
      strategy.FindOrderByExchangeOrderId(36028827892199865U);
  ASSERT_NE(by_exchange, nullptr);
  EXPECT_EQ(by_exchange->local_order_id, first.local_order_id);
}

TEST(StrategyTest, CancelsAcceptedOrderAndAppliesCancelResponse) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderCreateResult created = strategy.CreateLimitOrder(MakeLimitDraft());
  ASSERT_EQ(created.status, OrderCreateStatus::kOk);
  ASSERT_EQ(strategy.SubmitOrder(created.local_order_id).status,
            OrderSubmitStatus::kOk);
  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = created.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const OrderCancelResult cancel = strategy.CancelOrder(created.local_order_id);
  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kCancelAccepted,
      .local_order_id = created.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  EXPECT_EQ(cancel.status, OrderCancelStatus::kOk);
  EXPECT_EQ(gateway.cancel_calls, 1);
  EXPECT_EQ(gateway.last_cancel_local_order_id, created.local_order_id);
  const StrategyOrder* order = strategy.FindOrder(created.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelAccepted);
}

TEST(StrategyTest, GatewayCancelRejectedDoesNotEnterCancelSubmitted) {
  FakeGateway gateway;
  gateway.cancel_status = GatewaySendStatus::kRejected;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderCreateResult created = strategy.CreateLimitOrder(MakeLimitDraft());
  ASSERT_EQ(created.status, OrderCreateStatus::kOk);
  ASSERT_EQ(strategy.SubmitOrder(created.local_order_id).status,
            OrderSubmitStatus::kOk);
  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = created.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const OrderCancelResult cancel = strategy.CancelOrder(created.local_order_id);

  EXPECT_EQ(cancel.status, OrderCancelStatus::kGatewayRejected);
  EXPECT_EQ(gateway.cancel_calls, 1);
  const StrategyOrder* order = strategy.FindOrder(created.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kAccepted);
}

TEST(StrategyTest, DuplicateCancelIsRejectedByInvalidStatus) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderCreateResult created = strategy.CreateLimitOrder(MakeLimitDraft());
  ASSERT_EQ(created.status, OrderCreateStatus::kOk);
  ASSERT_EQ(strategy.SubmitOrder(created.local_order_id).status,
            OrderSubmitStatus::kOk);

  const OrderCancelResult first = strategy.CancelOrder(created.local_order_id);
  const OrderCancelResult second = strategy.CancelOrder(created.local_order_id);

  EXPECT_EQ(first.status, OrderCancelStatus::kOk);
  EXPECT_EQ(second.status, OrderCancelStatus::kInvalidStatus);
  EXPECT_EQ(gateway.cancel_calls, 1);
  const StrategyOrder* order = strategy.FindOrder(created.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSubmitted);
}

TEST(StrategyTest, DelayedAckAfterCancelSubmittedDoesNotOverwriteStatus) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderCreateResult created = strategy.CreateLimitOrder(MakeLimitDraft());
  ASSERT_EQ(created.status, OrderCreateStatus::kOk);
  ASSERT_EQ(strategy.SubmitOrder(created.local_order_id).status,
            OrderSubmitStatus::kOk);
  ASSERT_EQ(strategy.CancelOrder(created.local_order_id).status,
            OrderCancelStatus::kOk);

  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAck,
      .local_order_id = created.local_order_id,
  });

  const StrategyOrder* order = strategy.FindOrder(created.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSubmitted);
}

TEST(StrategyTest, DelayedAcceptedAfterCancelSubmittedBindsExchangeIdOnly) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderCreateResult created = strategy.CreateLimitOrder(MakeLimitDraft());
  ASSERT_EQ(created.status, OrderCreateStatus::kOk);
  ASSERT_EQ(strategy.SubmitOrder(created.local_order_id).status,
            OrderSubmitStatus::kOk);
  ASSERT_EQ(strategy.CancelOrder(created.local_order_id).status,
            OrderCancelStatus::kOk);

  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = created.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const StrategyOrder* order = strategy.FindOrder(created.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSubmitted);
  EXPECT_EQ(order->exchange_order_id, 36028827892199865U);
  const StrategyOrder* by_exchange =
      strategy.FindOrderByExchangeOrderId(36028827892199865U);
  ASSERT_NE(by_exchange, nullptr);
  EXPECT_EQ(by_exchange->local_order_id, created.local_order_id);
}

}  // namespace
}  // namespace aquila::strategy
