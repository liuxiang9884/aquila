#include "core/trading/strategy_context.h"

#include <cstdint>
#include <string_view>

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
  std::string_view last_place_price_text{};

  SendResult PlaceOrder(StrategyOrder& order) noexcept {
    ++place_calls;
    last_place_local_order_id = order.local_order_id;
    last_place_symbol = order.symbol;
    last_place_price_text = order.price_text;
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(StrategyOrder& order) noexcept {
    ++cancel_calls;
    last_cancel_local_order_id = order.local_order_id;
    return {.status = SendStatus::kOk};
  }
};

OrderCreateRequest MakeLimitRequest() noexcept {
  return OrderCreateRequest{.exchange = Exchange::kGate,
                            .symbol_id = 42,
                            .symbol = "BTC_USDT",
                            .side = OrderSide::kBuy,
                            .time_in_force = TimeInForce::kGoodTillCancel,
                            .quantity = 1,
                            .price_text = "65000",
                            .reduce_only = false};
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
  EXPECT_EQ(order_session.last_place_price_text, "65000");
  const StrategyOrder* order = context.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kSent);
}

TEST(StrategyContextTest,
     CancelOrderDelegatesThroughOrderManagerToOrderSession) {
  FakeOrderSession order_session;
  StrategyContext<FakeOrderSession>::OrderManagerT order_manager(order_session,
                                                                 8);
  StrategyContext<FakeOrderSession> context(order_manager);
  const OrderPlaceResult placed = context.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  const OrderCancelResult cancelled =
      context.CancelOrder(placed.local_order_id);

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

}  // namespace
}  // namespace aquila::core
