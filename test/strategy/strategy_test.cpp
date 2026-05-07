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

  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  SendStatus place_status{SendStatus::kOk};
  SendStatus cancel_status{SendStatus::kOk};
  int place_calls{0};
  int cancel_calls{0};
  std::int64_t last_place_local_order_id{0};
  std::int64_t last_cancel_local_order_id{0};

  SendResult PlaceOrder(Order& order) noexcept {
    ++place_calls;
    last_place_local_order_id = order.local_order_id;
    EXPECT_EQ(order.symbol, "BTC_USDT");
    EXPECT_EQ(order.price_text, "81000");
    return {.status = place_status};
  }

  SendResult CancelOrder(Order& order) noexcept {
    ++cancel_calls;
    last_cancel_local_order_id = order.local_order_id;
    return {.status = cancel_status};
  }
};

OrderCreateRequest MakeLimitRequest() noexcept {
  return OrderCreateRequest{.exchange = Exchange::kGate,
                            .symbol_id = 7,
                            .symbol = "BTC_USDT",
                            .side = OrderSide::kBuy,
                            .time_in_force = TimeInForce::kGoodTillCancel,
                            .signed_quantity = 1,
                            .price_text = "81000",
                            .reduce_only = false};
}

TEST(StrategyTest, PlacesLimitOrderAndStoresSubmittedOrder) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);

  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  EXPECT_EQ(placed.local_order_id, 1);
  EXPECT_EQ(gateway.place_calls, 1);
  EXPECT_EQ(gateway.last_place_local_order_id, placed.local_order_id);
  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kSubmitted);
  EXPECT_EQ(order->exchange, Exchange::kGate);
  EXPECT_EQ(order->symbol_id, 7);
  EXPECT_EQ(order->symbol, "BTC_USDT");
  EXPECT_EQ(order->signed_quantity, 1);
}

TEST(StrategyTest, RejectsInvalidLimitOrderBeforeAllocatingOrder) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  OrderCreateRequest request = MakeLimitRequest();
  request.price_text = "";

  const OrderPlaceResult placed = strategy.PlaceLimitOrder(request);

  EXPECT_EQ(placed.status, OrderPlaceStatus::kInvalidOrder);
  EXPECT_EQ(placed.local_order_id, 0);
  EXPECT_EQ(strategy.order_count(), 0U);
  EXPECT_EQ(gateway.place_calls, 0);
}

TEST(StrategyTest, SessionPlaceRejectedMarksOrderRejected) {
  FakeGateway gateway;
  gateway.place_status = FakeGateway::SendStatus::kRejected;
  Strategy<FakeGateway> strategy(gateway, 8);

  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());

  EXPECT_EQ(placed.status, OrderPlaceStatus::kSessionRejected);
  EXPECT_EQ(gateway.place_calls, 1);
  EXPECT_NE(placed.local_order_id, 0);
  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kRejected);
}

TEST(StrategyTest, AcceptedResponseStoresExchangeOrderIdIndex) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kAccepted);
  EXPECT_EQ(order->exchange_order_id, 36028827892199865U);
  const StrategyOrder* by_exchange =
      strategy.FindOrderByExchangeOrderId(36028827892199865U);
  ASSERT_NE(by_exchange, nullptr);
  EXPECT_EQ(by_exchange->local_order_id, placed.local_order_id);
}

TEST(StrategyTest, DuplicateExchangeOrderIdDoesNotAcceptSecondOrder) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderPlaceResult first = strategy.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(first.status, OrderPlaceStatus::kOk);
  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = first.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const OrderPlaceResult second = strategy.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(second.status, OrderPlaceStatus::kOk);
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
  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const OrderCancelResult cancel = strategy.CancelOrder(placed.local_order_id);
  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kCancelAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  EXPECT_EQ(cancel.status, OrderCancelStatus::kOk);
  EXPECT_EQ(gateway.cancel_calls, 1);
  EXPECT_EQ(gateway.last_cancel_local_order_id, placed.local_order_id);
  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelAccepted);
}

TEST(StrategyTest, SessionCancelRejectedDoesNotEnterCancelSubmitted) {
  FakeGateway gateway;
  gateway.cancel_status = FakeGateway::SendStatus::kRejected;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const OrderCancelResult cancel = strategy.CancelOrder(placed.local_order_id);

  EXPECT_EQ(cancel.status, OrderCancelStatus::kSessionRejected);
  EXPECT_EQ(gateway.cancel_calls, 1);
  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kAccepted);
}

TEST(StrategyTest, DuplicateCancelIsRejectedByInvalidStatus) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  const OrderCancelResult first = strategy.CancelOrder(placed.local_order_id);
  const OrderCancelResult second = strategy.CancelOrder(placed.local_order_id);

  EXPECT_EQ(first.status, OrderCancelStatus::kOk);
  EXPECT_EQ(second.status, OrderCancelStatus::kInvalidStatus);
  EXPECT_EQ(gateway.cancel_calls, 1);
  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSubmitted);
}

TEST(StrategyTest, DelayedAckAfterCancelSubmittedDoesNotOverwriteStatus) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(strategy.CancelOrder(placed.local_order_id).status,
            OrderCancelStatus::kOk);

  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAck,
      .local_order_id = placed.local_order_id,
  });

  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSubmitted);
}

TEST(StrategyTest, DelayedAcceptedAfterCancelSubmittedBindsExchangeIdOnly) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(strategy.CancelOrder(placed.local_order_id).status,
            OrderCancelStatus::kOk);

  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSubmitted);
  EXPECT_EQ(order->exchange_order_id, 36028827892199865U);
  const StrategyOrder* by_exchange =
      strategy.FindOrderByExchangeOrderId(36028827892199865U);
  ASSERT_NE(by_exchange, nullptr);
  EXPECT_EQ(by_exchange->local_order_id, placed.local_order_id);
}

}  // namespace
}  // namespace aquila::strategy
