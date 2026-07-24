#include "core/trading/strategy_context.h"

#include <cstdint>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/trading/order_manager.h"
#include "core/trading/order_types.h"

namespace aquila::core {
namespace {

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  int place_calls{0};
  int cancel_calls{0};
  std::uint64_t last_place_local_order_id{0};
  std::uint64_t last_cancel_local_order_id{0};
  std::string_view last_place_symbol{};
  double last_place_quantity{0.0};
  double last_place_price{0.0};
  OrderType last_place_order_type{OrderType::kLimit};

  SendResult PlaceOrder(const OrderPlaceRequest& order) noexcept {
    ++place_calls;
    last_place_local_order_id = order.local_order_id;
    last_place_symbol = order.SymbolView();
    last_place_quantity = order.quantity;
    last_place_price = order.price;
    last_place_order_type = order.order_type;
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(const OrderCancelRequest& order) noexcept {
    ++cancel_calls;
    last_cancel_local_order_id = order.local_order_id;
    return {.status = SendStatus::kOk};
  }
};

static_assert(
    std::is_same_v<decltype(&StrategyContext<FakeOrderSession>::PlaceOrder),
                   OrderPlaceResult (StrategyContext<FakeOrderSession>::*)(
                       const OrderPlaceRequest&, OrderLocalMetadata) noexcept>);

OrderPlaceRequest MakeLimitRequest() noexcept {
  OrderPlaceRequest request{.price = 65000.0,
                            .quantity = 1.0,
                            .symbol_id = 42,
                            .exchange = Exchange::kGate,
                            .side = OrderSide::kBuy,
                            .order_type = OrderType::kLimit,
                            .time_in_force = TimeInForce::kGoodTillCancel,
                            .price_decimal_places = 0,
                            .quantity_decimal_places = 0,
                            .reduce_only = false};
  SetOrderSymbol(&request, "BTC_USDT");
  return request;
}

TEST(StrategyContextTest,
     PlaceLimitOrderDelegatesThroughOrderManagerToOrderSession) {
  FakeOrderSession order_session;
  StrategyContext<FakeOrderSession>::OrderManagerT order_manager(order_session,
                                                                 8);
  StrategyContext<FakeOrderSession> context(order_manager);

  const OrderPlaceResult placed = context.PlaceLimitOrder(MakeLimitRequest());

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  EXPECT_EQ(order_session.place_calls, 1);
  EXPECT_EQ(order_session.last_place_local_order_id, placed.local_order_id);
  EXPECT_EQ(order_session.last_place_symbol, "BTC_USDT");
  EXPECT_DOUBLE_EQ(order_session.last_place_quantity, 1.0);
  EXPECT_DOUBLE_EQ(order_session.last_place_price, 65000.0);
  const StrategyOrder* order = context.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kSent);
}

TEST(StrategyContextTest, PlaceOrderCopiesGenericOrderTypeToStrategyOrder) {
  FakeOrderSession order_session;
  StrategyContext<FakeOrderSession>::OrderManagerT order_manager(order_session,
                                                                 8);
  StrategyContext<FakeOrderSession> context(order_manager);
  OrderPlaceRequest request = MakeLimitRequest();
  request.order_type = OrderType::kMarket;

  const OrderPlaceResult placed = context.PlaceOrder(request);

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  EXPECT_EQ(order_session.last_place_order_type, OrderType::kMarket);
  const StrategyOrder* order = context.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->place_request.order_type, OrderType::kMarket);
}

TEST(StrategyContextTest, PlaceOrderCopiesLocalMetadataToStrategyOrder) {
  FakeOrderSession order_session;
  StrategyContext<FakeOrderSession>::OrderManagerT order_manager(order_session,
                                                                 8);
  StrategyContext<FakeOrderSession> context(order_manager);

  const OrderPlaceResult placed = context.PlaceOrder(
      MakeLimitRequest(), OrderLocalMetadata{.group_index = 3});

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  const StrategyOrder* order = context.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->group_index, 3);
}

TEST(StrategyContextTest, PlaceLimitOrderForcesLimitOrderType) {
  FakeOrderSession order_session;
  StrategyContext<FakeOrderSession>::OrderManagerT order_manager(order_session,
                                                                 8);
  StrategyContext<FakeOrderSession> context(order_manager);
  OrderPlaceRequest request = MakeLimitRequest();
  request.order_type = OrderType::kMarket;

  const OrderPlaceResult placed = context.PlaceLimitOrder(request);

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  EXPECT_EQ(order_session.last_place_order_type, OrderType::kLimit);
  const StrategyOrder* order = context.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->place_request.order_type, OrderType::kLimit);
}

TEST(StrategyContextTest,
     CancelOrderDelegatesThroughOrderManagerToOrderSession) {
  FakeOrderSession order_session;
  StrategyContext<FakeOrderSession>::OrderManagerT order_manager(order_session,
                                                                 8);
  StrategyContext<FakeOrderSession> context(order_manager);
  const OrderPlaceResult placed = context.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  const OrderCancelResult cancelled = context.CancelOrder(
      OrderCancelRequest{.local_order_id = placed.local_order_id});

  ASSERT_EQ(cancelled.status, OrderCancelStatus::kOk);
  EXPECT_EQ(cancelled.local_order_id, placed.local_order_id);
  EXPECT_EQ(order_session.cancel_calls, 1);
  EXPECT_EQ(order_session.last_cancel_local_order_id, placed.local_order_id);
  const StrategyOrder* order = context.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kCancelSent);
}

TEST(StrategyContextTest, FindOrderReturnsManagedOrderPointer) {
  FakeOrderSession order_session;
  StrategyContext<FakeOrderSession>::OrderManagerT order_manager(order_session,
                                                                 8);
  StrategyContext<FakeOrderSession> context(order_manager);
  const OrderPlaceResult placed = context.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  const StrategyOrder* expected =
      order_manager.FindOrder(placed.local_order_id);
  const StrategyOrder* actual = context.FindOrder(placed.local_order_id);

  ASSERT_NE(expected, nullptr);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(context.FindOrder(999999U), nullptr);
}

TEST(StrategyContextTest, RetireFinishedOrderDelegatesToOrderManager) {
  FakeOrderSession order_session;
  StrategyContext<FakeOrderSession>::OrderManagerT order_manager(order_session,
                                                                 8);
  StrategyContext<FakeOrderSession> context(order_manager);
  const OrderPlaceResult placed = context.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  order_manager.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kFilled,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865ULL,
      .cumulative_filled_quantity = 1,
      .left_quantity = 0,
      .cancelled_quantity = 0,
      .fill_price = 65000.0,
      .role = OrderRole::kTaker,
      .finish_reason = OrderFinishReason::kUnknown,
      .reject_reason = OrderRejectReason::kUnknown,
      .continuity_scope = OrderFeedbackContinuityScope::kLane,
      .continuity_reason = OrderFeedbackContinuityReason::kUnknown,
      .continuity_sequence = 0,
      .exchange_update_ns = 100,
      .local_receive_ns = 101,
  });
  ASSERT_NE(context.FindOrder(placed.local_order_id), nullptr);

  EXPECT_TRUE(context.RetireFinishedOrder(placed.local_order_id));

  EXPECT_EQ(context.FindOrder(placed.local_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);
}

}  // namespace
}  // namespace aquila::core
