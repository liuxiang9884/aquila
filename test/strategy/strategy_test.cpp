#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_id.h"
#include "core/trading/order_manager.h"
#include "core/trading/order_types.h"

namespace aquila::core {
namespace {

template <typename T>
concept HasErrorLabelHash = requires(T value) { value.error_label_hash; };

static_assert(!HasErrorLabelHash<OrderResponseEvent>);
static_assert(!HasErrorLabelHash<StrategyOrder>);

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
  int cache_update_calls{0};
  int cache_forget_calls{0};
  std::uint64_t last_place_local_order_id{0};
  std::uint64_t last_cancel_local_order_id{0};
  std::uint64_t last_cache_local_order_id{0};
  std::uint64_t last_cache_exchange_order_id{0};
  std::uint64_t last_forget_local_order_id{0};
  std::string_view expected_quantity_text{"1"};

  SendResult PlaceOrder(Order& order) noexcept {
    ++place_calls;
    last_place_local_order_id = order.local_order_id;
    EXPECT_EQ(order.symbol, "BTC_USDT");
    EXPECT_EQ(order.quantity_text, expected_quantity_text);
    EXPECT_EQ(order.price_text, "81000");
    return {.status = place_status};
  }

  SendResult CancelOrder(Order& order) noexcept {
    ++cancel_calls;
    last_cancel_local_order_id = order.local_order_id;
    return {.status = cancel_status};
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    ++cache_update_calls;
    last_cache_local_order_id = local_order_id;
    last_cache_exchange_order_id = exchange_order_id;
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    ++cache_forget_calls;
    last_forget_local_order_id = local_order_id;
  }
};

OrderCreateRequest MakeLimitRequest() noexcept {
  return OrderCreateRequest{.exchange = Exchange::kGate,
                            .symbol_id = 7,
                            .symbol = "BTC_USDT",
                            .side = OrderSide::kBuy,
                            .time_in_force = TimeInForce::kGoodTillCancel,
                            .quantity = 1,
                            .quantity_text = "1",
                            .price_text = "81000",
                            .reduce_only = false};
}

TEST(OrderManagerTest, PlacesLimitOrderAndStoresSentOrder) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);

  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  EXPECT_EQ(placed.local_order_id, 1);
  EXPECT_EQ(gateway.place_calls, 1);
  EXPECT_EQ(gateway.last_place_local_order_id, placed.local_order_id);
  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kSent);
  EXPECT_EQ(order->exchange, Exchange::kGate);
  EXPECT_EQ(order->symbol_id, 7);
  EXPECT_EQ(order->symbol, "BTC_USDT");
  EXPECT_EQ(order->quantity, 1);
  EXPECT_EQ(order->quantity_text, "1");
}

TEST(OrderManagerTest, PlacesDecimalQuantityWithStableQuantityText) {
  FakeGateway gateway;
  gateway.expected_quantity_text = "0.1";
  OrderManager<FakeGateway> order_manager(gateway, 8);
  OrderCreateRequest request = MakeLimitRequest();
  request.quantity = 0.1;
  request.quantity_text = "0.1";

  const OrderPlaceResult placed = order_manager.PlaceLimitOrder(request);

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  EXPECT_EQ(gateway.place_calls, 1);
  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_DOUBLE_EQ(order->quantity, 0.1);
  EXPECT_EQ(order->quantity_text, "0.1");
}

TEST(OrderManagerTest, EncodesStrategyIdIntoCreatedLocalOrderId) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8, 7);

  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  EXPECT_EQ(LocalOrderIdCodec::StrategyId(placed.local_order_id), 7);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(placed.local_order_id), 1U);
  EXPECT_EQ(gateway.last_place_local_order_id, placed.local_order_id);
  EXPECT_NE(order_manager.FindOrder(placed.local_order_id), nullptr);
}

TEST(OrderManagerTest, AckResponseKeepsSentOrderUntilFinalResult) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  order_manager.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAck,
      .local_order_id = placed.local_order_id,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kSent);
}

TEST(OrderManagerTest, RejectsInvalidLimitOrderBeforeAllocatingOrder) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  OrderCreateRequest request = MakeLimitRequest();
  request.price_text = "";

  const OrderPlaceResult placed = order_manager.PlaceLimitOrder(request);

  EXPECT_EQ(placed.status, OrderPlaceStatus::kInvalidOrder);
  EXPECT_EQ(placed.local_order_id, 0);
  EXPECT_EQ(order_manager.order_count(), 0U);
  EXPECT_EQ(gateway.place_calls, 0);
}

TEST(OrderManagerTest, RejectsNonPositiveQuantityBeforeAllocatingOrder) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  OrderCreateRequest request = MakeLimitRequest();
  request.quantity = 0;

  const OrderPlaceResult placed = order_manager.PlaceLimitOrder(request);

  EXPECT_EQ(placed.status, OrderPlaceStatus::kInvalidOrder);
  EXPECT_EQ(placed.local_order_id, 0);
  EXPECT_EQ(order_manager.order_count(), 0U);
  EXPECT_EQ(gateway.place_calls, 0);
}

TEST(OrderManagerTest, SessionPlaceRejectedMarksOrderRejected) {
  FakeGateway gateway;
  gateway.place_status = FakeGateway::SendStatus::kRejected;
  OrderManager<FakeGateway> order_manager(gateway, 8);

  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());

  EXPECT_EQ(placed.status, OrderPlaceStatus::kSessionRejected);
  EXPECT_EQ(gateway.place_calls, 1);
  EXPECT_NE(placed.local_order_id, 0);
  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kRejected);
}

TEST(OrderManagerTest, AcceptedResponseMarksSentOrderAccepted) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  order_manager.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kAccepted);
}

TEST(OrderManagerTest, CancelsAcceptedOrderAndAppliesCancelResponse) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  order_manager.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const OrderCancelResult cancel =
      order_manager.CancelOrder(placed.local_order_id);
  order_manager.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kCancelAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  EXPECT_EQ(cancel.status, OrderCancelStatus::kOk);
  EXPECT_EQ(gateway.cancel_calls, 1);
  EXPECT_EQ(gateway.last_cancel_local_order_id, placed.local_order_id);
  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelled);
}

TEST(OrderManagerTest, SessionCancelRejectedDoesNotEnterCancelSent) {
  FakeGateway gateway;
  gateway.cancel_status = FakeGateway::SendStatus::kRejected;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  order_manager.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const OrderCancelResult cancel =
      order_manager.CancelOrder(placed.local_order_id);

  EXPECT_EQ(cancel.status, OrderCancelStatus::kSessionRejected);
  EXPECT_EQ(gateway.cancel_calls, 1);
  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kAccepted);
}

TEST(OrderManagerTest, CancelRejectedResponseMarksOrderRejected) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  order_manager.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });
  ASSERT_EQ(order_manager.CancelOrder(placed.local_order_id).status,
            OrderCancelStatus::kOk);

  order_manager.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kCancelRejected,
      .local_order_id = placed.local_order_id,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kRejected);
}

TEST(OrderManagerTest, DuplicateCancelIsRejectedByInvalidStatus) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  const OrderCancelResult first =
      order_manager.CancelOrder(placed.local_order_id);
  const OrderCancelResult second =
      order_manager.CancelOrder(placed.local_order_id);

  EXPECT_EQ(first.status, OrderCancelStatus::kOk);
  EXPECT_EQ(second.status, OrderCancelStatus::kInvalidStatus);
  EXPECT_EQ(gateway.cancel_calls, 1);
  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSent);
}

TEST(OrderManagerTest, DelayedAckAfterCancelSentDoesNotOverwriteStatus) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(order_manager.CancelOrder(placed.local_order_id).status,
            OrderCancelStatus::kOk);

  order_manager.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAck,
      .local_order_id = placed.local_order_id,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSent);
}

TEST(OrderManagerTest, DelayedAcceptedAfterCancelSentKeepsCancelSent) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(order_manager.CancelOrder(placed.local_order_id).status,
            OrderCancelStatus::kOk);

  order_manager.OnOrderResponse(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSent);
}

TEST(OrderManagerFeedbackTest,
     AcceptedFeedbackMarksSentOrderAcceptedAndCachesId) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
      .exchange_update_ns = 1234567890,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kAccepted);
  EXPECT_EQ(order->exchange_order_id, 36028827892199865U);
  EXPECT_EQ(order->exchange_update_ns, 1234567890);
  EXPECT_EQ(gateway.cache_update_calls, 1);
  EXPECT_EQ(gateway.last_cache_local_order_id, placed.local_order_id);
  EXPECT_EQ(gateway.last_cache_exchange_order_id, 36028827892199865U);
}

TEST(OrderManagerFeedbackTest, PartialFilledFeedbackUpdatesCumulativeAverage) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  OrderCreateRequest request = MakeLimitRequest();
  request.quantity = 5;
  const OrderPlaceResult placed = order_manager.PlaceLimitOrder(request);
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kPartialFilled,
      .local_order_id = placed.local_order_id,
      .cumulative_filled_quantity = 3,
      .left_quantity = 2,
      .fill_price = 81010.0,
      .exchange_update_ns = 2000,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kPartialFilled);
  EXPECT_EQ(order->cumulative_filled_quantity, 3);
  EXPECT_DOUBLE_EQ(order->cumulative_filled_value, 243030.0);
  EXPECT_DOUBLE_EQ(order->AverageFillPrice(), 81010.0);
  EXPECT_DOUBLE_EQ(order->last_fill_price, 81010.0);
  EXPECT_EQ(order->exchange_update_ns, 2000);
  EXPECT_FALSE(order->is_finished);
}

TEST(OrderManagerFeedbackTest, PartialFillAfterCancelSentKeepsCancelPending) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  OrderCreateRequest request = MakeLimitRequest();
  request.quantity = 5;
  const OrderPlaceResult placed = order_manager.PlaceLimitOrder(request);
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
      .exchange_update_ns = 1000,
  });
  ASSERT_EQ(order_manager.CancelOrder(placed.local_order_id).status,
            OrderCancelStatus::kOk);

  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kPartialFilled,
      .local_order_id = placed.local_order_id,
      .cumulative_filled_quantity = 2,
      .left_quantity = 3,
      .fill_price = 81010.0,
      .exchange_update_ns = 2000,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSent);
  EXPECT_EQ(order->cumulative_filled_quantity, 2);
  EXPECT_DOUBLE_EQ(order->AverageFillPrice(), 81010.0);
  EXPECT_EQ(order->exchange_update_ns, 2000);
}

TEST(OrderManagerFeedbackTest,
     RepeatedPartialWithSameCumulativeQuantityIsIgnored) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  OrderCreateRequest request = MakeLimitRequest();
  request.quantity = 5;
  const OrderPlaceResult placed = order_manager.PlaceLimitOrder(request);
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kPartialFilled,
      .local_order_id = placed.local_order_id,
      .cumulative_filled_quantity = 3,
      .left_quantity = 2,
      .fill_price = 81010.0,
      .exchange_update_ns = 2000,
  });
  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kPartialFilled,
      .local_order_id = placed.local_order_id,
      .cumulative_filled_quantity = 3,
      .left_quantity = 2,
      .fill_price = 82000.0,
      .exchange_update_ns = 3000,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kPartialFilled);
  EXPECT_EQ(order->cumulative_filled_quantity, 3);
  EXPECT_DOUBLE_EQ(order->cumulative_filled_value, 243030.0);
  EXPECT_DOUBLE_EQ(order->AverageFillPrice(), 81010.0);
  EXPECT_EQ(order->exchange_update_ns, 2000);
  EXPECT_EQ(order_manager.feedback_stats().duplicate_or_stale_feedbacks, 1U);
}

TEST(OrderManagerFeedbackTest, FilledFeedbackFinalizesOrderAndIsIdempotent) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  OrderCreateRequest request = MakeLimitRequest();
  request.quantity = 5;
  const OrderPlaceResult placed = order_manager.PlaceLimitOrder(request);
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kPartialFilled,
      .local_order_id = placed.local_order_id,
      .cumulative_filled_quantity = 2,
      .left_quantity = 3,
      .fill_price = 80000.0,
      .exchange_update_ns = 1000,
  });
  const OrderFeedbackEvent filled{
      .kind = OrderFeedbackKind::kFilled,
      .local_order_id = placed.local_order_id,
      .cumulative_filled_quantity = 5,
      .fill_price = 80200.0,
      .role = OrderRole::kMaker,
      .exchange_update_ns = 4000,
  };
  order_manager.OnOrderFeedback(filled);
  order_manager.OnOrderFeedback(filled);

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kFilled);
  EXPECT_TRUE(order->is_finished);
  EXPECT_EQ(order->cumulative_filled_quantity, 5);
  EXPECT_DOUBLE_EQ(order->cumulative_filled_value, 401000.0);
  EXPECT_DOUBLE_EQ(order->AverageFillPrice(), 80200.0);
  EXPECT_EQ(order->role, OrderRole::kMaker);
  EXPECT_EQ(order->exchange_update_ns, 4000);
  EXPECT_EQ(gateway.cache_forget_calls, 1);
  EXPECT_EQ(gateway.last_forget_local_order_id, placed.local_order_id);
  EXPECT_EQ(order_manager.feedback_stats().terminal_feedbacks_ignored, 1U);
}

TEST(OrderManagerFeedbackTest, CancelledFeedbackWithoutFillMarksCancelled) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kCancelled,
      .local_order_id = placed.local_order_id,
      .cumulative_filled_quantity = 0,
      .cancelled_quantity = 1,
      .finish_reason = OrderFinishReason::kManualCancelled,
      .exchange_update_ns = 5000,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelled);
  EXPECT_TRUE(order->is_finished);
  EXPECT_EQ(order->cumulative_filled_quantity, 0);
  EXPECT_DOUBLE_EQ(order->cumulative_filled_value, 0.0);
  EXPECT_EQ(order->finish_reason, OrderFinishReason::kManualCancelled);
  EXPECT_EQ(order->exchange_update_ns, 5000);
  EXPECT_EQ(gateway.cache_forget_calls, 1);
}

TEST(OrderManagerFeedbackTest,
     CancelledFeedbackWithFillMarksPartiallyCancelled) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  OrderCreateRequest request = MakeLimitRequest();
  request.quantity = 5;
  const OrderPlaceResult placed = order_manager.PlaceLimitOrder(request);
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kCancelled,
      .local_order_id = placed.local_order_id,
      .cumulative_filled_quantity = 2,
      .cancelled_quantity = 3,
      .fill_price = 80500.0,
      .finish_reason = OrderFinishReason::kImmediateOrCancel,
      .exchange_update_ns = 6000,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kPartiallyCancelled);
  EXPECT_TRUE(order->is_finished);
  EXPECT_EQ(order->cumulative_filled_quantity, 2);
  EXPECT_DOUBLE_EQ(order->cumulative_filled_value, 161000.0);
  EXPECT_DOUBLE_EQ(order->AverageFillPrice(), 80500.0);
  EXPECT_EQ(order->finish_reason, OrderFinishReason::kImmediateOrCancel);
  EXPECT_EQ(gateway.cache_forget_calls, 1);
}

TEST(OrderManagerFeedbackTest, RejectedFeedbackMarksOrderRejected) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kRejected,
      .local_order_id = placed.local_order_id,
      .reject_reason = OrderRejectReason::kExchangeRejected,
      .exchange_update_ns = 7000,
  });

  const StrategyOrder* order = order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kRejected);
  EXPECT_TRUE(order->is_finished);
  EXPECT_EQ(order->reject_reason, OrderRejectReason::kExchangeRejected);
  EXPECT_EQ(order->exchange_update_ns, 7000);
}

TEST(OrderManagerFeedbackTest, UnknownLocalOrderIdIncrementsDiagnostics) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);

  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kAccepted,
      .local_order_id = 999,
      .exchange_order_id = 36028827892199865U,
  });

  EXPECT_EQ(order_manager.feedback_stats().unknown_local_order_feedbacks, 1U);
  EXPECT_EQ(gateway.cache_update_calls, 0);
}

TEST(OrderManagerFeedbackTest,
     ContinuityLostFeedbackSetsContinuityLostDetected) {
  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8);

  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kContinuityLost,
      .continuity_scope = OrderFeedbackContinuityScope::kGlobal,
      .continuity_reason =
          OrderFeedbackContinuityReason::kReconnectUnknownWindow,
      .continuity_sequence = 42,
  });

  EXPECT_TRUE(order_manager.feedback_continuity_lost_detected());
  EXPECT_EQ(order_manager.feedback_stats().feedback_continuity_lost_events, 1U);
}

}  // namespace
}  // namespace aquila::core
