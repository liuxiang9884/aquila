#include "strategy/strategy.h"

#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/trading/order_id.h"
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
  std::uint64_t last_place_local_order_id{0};
  std::uint64_t last_cancel_local_order_id{0};

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
                            .quantity = 1,
                            .price_text = "81000",
                            .reduce_only = false};
}

TEST(StrategyTest, PlacesLimitOrderAndStoresSentOrder) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);

  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  EXPECT_EQ(placed.local_order_id, 1);
  EXPECT_EQ(gateway.place_calls, 1);
  EXPECT_EQ(gateway.last_place_local_order_id, placed.local_order_id);
  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kSent);
  EXPECT_EQ(order->exchange, Exchange::kGate);
  EXPECT_EQ(order->symbol_id, 7);
  EXPECT_EQ(order->symbol, "BTC_USDT");
  EXPECT_EQ(order->quantity, 1);
}

TEST(StrategyTest, EncodesStrategyIdIntoCreatedLocalOrderId) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8, 7);

  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  EXPECT_EQ(LocalOrderIdCodec::StrategyId(placed.local_order_id), 7);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(placed.local_order_id), 1U);
  EXPECT_EQ(gateway.last_place_local_order_id, placed.local_order_id);
  EXPECT_NE(strategy.FindOrder(placed.local_order_id), nullptr);
}

TEST(StrategyTest, AckResponseKeepsSentOrderUntilFinalResult) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAck,
      .local_order_id = placed.local_order_id,
  });

  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kSent);
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

TEST(StrategyTest, RejectsNonPositiveQuantityBeforeAllocatingOrder) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  OrderCreateRequest request = MakeLimitRequest();
  request.quantity = 0;

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

TEST(StrategyTest, AcceptedResponseMarksSentOrderAccepted) {
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
  EXPECT_EQ(order->status, OrderStatus::kCancelled);
}

TEST(StrategyTest, SessionCancelRejectedDoesNotEnterCancelSent) {
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

TEST(StrategyTest, CancelRejectedResponseMarksOrderRejected) {
  FakeGateway gateway;
  Strategy<FakeGateway> strategy(gateway, 8);
  const OrderPlaceResult placed = strategy.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });
  ASSERT_EQ(strategy.CancelOrder(placed.local_order_id).status,
            OrderCancelStatus::kOk);

  strategy.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kCancelRejected,
      .local_order_id = placed.local_order_id,
      .error_label_hash = 91U,
  });

  const StrategyOrder* order = strategy.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kRejected);
  EXPECT_EQ(order->error_label_hash, 91U);
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
  EXPECT_EQ(order->status, OrderStatus::kCancelSent);
}

TEST(StrategyTest, DelayedAckAfterCancelSentDoesNotOverwriteStatus) {
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
  EXPECT_EQ(order->status, OrderStatus::kCancelSent);
}

TEST(StrategyTest, DelayedAcceptedAfterCancelSentKeepsCancelSent) {
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
  EXPECT_EQ(order->status, OrderStatus::kCancelSent);
}

}  // namespace
}  // namespace aquila::strategy
