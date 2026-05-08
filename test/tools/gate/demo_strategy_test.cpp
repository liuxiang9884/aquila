#include "tools/gate/demo_strategy.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "core/strategy/order_manager.h"
#include "core/strategy/order_types.h"
#include "core/strategy/strategy_context.h"
#include "core/trading/order_feedback_event.h"

namespace aquila::tools::gate_demo_strategy {
namespace {

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  struct PlacedOrder {
    std::uint64_t local_order_id{0};
    std::int32_t symbol_id{0};
    std::string symbol;
    OrderSide side{OrderSide::kBuy};
    TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
    std::int64_t quantity{0};
    std::string price_text;
    bool reduce_only{false};
  };

  SendResult PlaceOrder(const strategy::StrategyOrder& order) {
    placed_orders.push_back(PlacedOrder{
        .local_order_id = order.local_order_id,
        .symbol_id = order.symbol_id,
        .symbol = std::string(order.symbol),
        .side = order.side,
        .time_in_force = order.time_in_force,
        .quantity = order.quantity,
        .price_text = std::string(order.price_text),
        .reduce_only = order.reduce_only,
    });
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(const strategy::StrategyOrder& order) noexcept {
    ++cancel_calls;
    last_cancel_local_order_id = order.local_order_id;
    return {.status = SendStatus::kOk};
  }

  std::vector<PlacedOrder> placed_orders;
  int cancel_calls{0};
  std::uint64_t last_cancel_local_order_id{0};
};

using OrderManagerT = strategy::OrderManager<FakeOrderSession>;
using ContextT = strategy::StrategyContext<FakeOrderSession>;

BookTicker MakeTicker(std::int32_t symbol_id = 7,
                      double ask_price = 81000.5) noexcept {
  return BookTicker{.id = 42,
                    .symbol_id = symbol_id,
                    .exchange = Exchange::kGate,
                    .exchange_ns = 1000,
                    .local_ns = 2000,
                    .bid_price = 81000.0,
                    .bid_volume = 3.0,
                    .ask_price = ask_price,
                    .ask_volume = 4.0};
}

OrderFeedbackEvent MakeFeedback(OrderFeedbackKind kind,
                                std::uint64_t local_order_id) noexcept {
  return OrderFeedbackEvent{
      .kind = kind,
      .local_order_id = local_order_id,
      .exchange_order_id = 36028827892199865U,
      .cumulative_filled_quantity = kind == OrderFeedbackKind::kFilled ? 1 : 0,
      .left_quantity = 0,
      .cancelled_quantity = kind == OrderFeedbackKind::kCancelled ? 1 : 0,
      .fill_price = kind == OrderFeedbackKind::kFilled ? 81000.5 : 0.0,
      .role = OrderRole::kTaker,
      .finish_reason = OrderFinishReason::kUnknown,
      .reject_reason = OrderRejectReason::kUnknown,
      .gap_scope = OrderFeedbackGapScope::kLane,
      .gap_reason = OrderFeedbackGapReason::kUnknown,
      .gap_sequence = 0,
      .exchange_update_ns = 1234567890,
      .local_receive_ns = 1234567999,
  };
}

void ApplyFeedback(OrderManagerT& order_manager, DemoStrategy& strategy,
                   ContextT& context, OrderFeedbackEvent event) {
  order_manager.OnOrderFeedback(event);
  strategy.OnOrderFeedback(event, context);
}

DemoStrategyConfig MakeConfig(std::uint32_t cycles = 1) {
  return DemoStrategyConfig{
      .contract = "BTC_USDT",
      .symbol_id = 7,
      .wait_minutes = 0,
      .cycles = cycles,
  };
}

TEST(DemoStrategyTest, ParsesDemoConfigFromTomlTable) {
  toml::table table{
      {"demo",
       toml::table{
           {"contract", "ETH_USDT"},
           {"symbol_id", 11},
           {"wait_minutes", 3},
           {"cycles", 4},
       }},
  };

  const Result<DemoStrategyConfig> parsed = ParseDemoStrategyConfig(table);

  ASSERT_TRUE(parsed.ok) << parsed.error;
  EXPECT_EQ(parsed.value.contract, "ETH_USDT");
  EXPECT_EQ(parsed.value.symbol_id, 11);
  EXPECT_EQ(parsed.value.wait_minutes, 3U);
  EXPECT_EQ(parsed.value.cycles, 4U);
}

TEST(DemoStrategyTest, PlacesBuyLimitAtAskForMatchingSymbol) {
  FakeOrderSession session;
  OrderManagerT order_manager(session, 8);
  ContextT context(order_manager);
  DemoStrategy strategy(MakeConfig());

  strategy.OnBookTicker(MakeTicker(), context);

  ASSERT_EQ(session.placed_orders.size(), 1U);
  const FakeOrderSession::PlacedOrder& buy = session.placed_orders.back();
  EXPECT_EQ(buy.symbol, "BTC_USDT");
  EXPECT_EQ(buy.symbol_id, 7);
  EXPECT_EQ(buy.side, OrderSide::kBuy);
  EXPECT_EQ(buy.time_in_force, TimeInForce::kGoodTillCancel);
  EXPECT_EQ(buy.quantity, 1);
  EXPECT_EQ(buy.price_text, "81000.5");
  EXPECT_FALSE(buy.reduce_only);
}

TEST(DemoStrategyTest, IgnoresTickerForDifferentSymbol) {
  FakeOrderSession session;
  OrderManagerT order_manager(session, 8);
  ContextT context(order_manager);
  DemoStrategy strategy(MakeConfig());

  strategy.OnBookTicker(MakeTicker(99), context);

  EXPECT_TRUE(session.placed_orders.empty());
  EXPECT_FALSE(strategy.ShouldStop());
}

TEST(DemoStrategyTest, ClosesWithReduceOnlyMarketSellAfterFilledBuyExpires) {
  FakeOrderSession session;
  OrderManagerT order_manager(session, 8);
  ContextT context(order_manager);
  DemoStrategy strategy(MakeConfig());
  strategy.OnBookTicker(MakeTicker(), context);
  ASSERT_EQ(session.placed_orders.size(), 1U);
  const std::uint64_t buy_local_order_id =
      session.placed_orders.back().local_order_id;

  ApplyFeedback(order_manager, strategy, context,
                MakeFeedback(OrderFeedbackKind::kFilled, buy_local_order_id));
  strategy.OnIdle(context);

  ASSERT_EQ(session.placed_orders.size(), 2U);
  const FakeOrderSession::PlacedOrder& close = session.placed_orders.back();
  EXPECT_EQ(close.symbol, "BTC_USDT");
  EXPECT_EQ(close.symbol_id, 7);
  EXPECT_EQ(close.side, OrderSide::kSell);
  EXPECT_EQ(close.time_in_force, TimeInForce::kImmediateOrCancel);
  EXPECT_EQ(close.quantity, 1);
  EXPECT_EQ(close.price_text, "0");
  EXPECT_TRUE(close.reduce_only);
}

TEST(DemoStrategyTest, CancelsUnfilledBuyAfterWaitExpires) {
  FakeOrderSession session;
  OrderManagerT order_manager(session, 8);
  ContextT context(order_manager);
  DemoStrategy strategy(MakeConfig());
  strategy.OnBookTicker(MakeTicker(), context);
  ASSERT_EQ(session.placed_orders.size(), 1U);
  const std::uint64_t buy_local_order_id =
      session.placed_orders.back().local_order_id;

  strategy.OnIdle(context);

  EXPECT_EQ(session.cancel_calls, 1);
  EXPECT_EQ(session.last_cancel_local_order_id, buy_local_order_id);
}

TEST(DemoStrategyTest, KeepsHistoricalBuyPriceTextStableAcrossCycles) {
  FakeOrderSession session;
  OrderManagerT order_manager(session, 8);
  ContextT context(order_manager);
  DemoStrategy strategy(MakeConfig(2));

  strategy.OnBookTicker(MakeTicker(7, 81000.5), context);
  ASSERT_EQ(session.placed_orders.size(), 1U);
  const std::uint64_t first_buy = session.placed_orders.back().local_order_id;
  strategy.OnIdle(context);
  ApplyFeedback(order_manager, strategy, context,
                MakeFeedback(OrderFeedbackKind::kCancelled, first_buy));

  strategy.OnBookTicker(MakeTicker(7, 81001.5), context);

  const strategy::StrategyOrder* first_order = context.FindOrder(first_buy);
  ASSERT_NE(first_order, nullptr);
  EXPECT_EQ(first_order->price_text, "81000.5");
  ASSERT_EQ(session.placed_orders.size(), 2U);
  EXPECT_EQ(session.placed_orders.back().price_text, "81001.5");
}

TEST(DemoStrategyTest, StopsAfterConfiguredCyclesComplete) {
  FakeOrderSession session;
  OrderManagerT order_manager(session, 8);
  ContextT context(order_manager);
  DemoStrategy strategy(MakeConfig(2));

  strategy.OnBookTicker(MakeTicker(7, 81000.5), context);
  ASSERT_EQ(session.placed_orders.size(), 1U);
  const std::uint64_t first_buy = session.placed_orders.back().local_order_id;
  strategy.OnIdle(context);
  ApplyFeedback(order_manager, strategy, context,
                MakeFeedback(OrderFeedbackKind::kCancelled, first_buy));
  EXPECT_FALSE(strategy.ShouldStop());

  strategy.OnBookTicker(MakeTicker(7, 81001.5), context);
  ASSERT_EQ(session.placed_orders.size(), 2U);
  const std::uint64_t second_buy = session.placed_orders.back().local_order_id;
  strategy.OnIdle(context);
  ApplyFeedback(order_manager, strategy, context,
                MakeFeedback(OrderFeedbackKind::kCancelled, second_buy));

  EXPECT_TRUE(strategy.ShouldStop());
}

}  // namespace
}  // namespace aquila::tools::gate_demo_strategy
