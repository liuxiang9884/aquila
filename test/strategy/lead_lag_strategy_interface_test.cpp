#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/config/strategy_config.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "core/trading/trading_runtime.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/signal.h"
#include "strategy/lead_lag/strategy.h"
#include "strategy/lead_lag/types.h"

namespace {

namespace leadlag = aquila::strategy::leadlag;

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kOk};
  };

  struct CapturedOrder {
    std::uint64_t local_order_id{0};
    aquila::Exchange exchange{aquila::Exchange::kGate};
    std::int32_t symbol_id{0};
    std::string symbol;
    aquila::OrderSide side{aquila::OrderSide::kBuy};
    aquila::OrderType order_type{aquila::OrderType::kLimit};
    aquila::TimeInForce time_in_force{aquila::TimeInForce::kGoodTillCancel};
    std::int64_t quantity{0};
    std::string price_text;
    bool reduce_only{false};
  };

  SendResult PlaceOrder(aquila::core::StrategyOrder& order) noexcept {
    placed_orders.push_back(CapturedOrder{
        .local_order_id = order.local_order_id,
        .exchange = order.exchange,
        .symbol_id = order.symbol_id,
        .symbol = std::string(order.symbol),
        .side = order.side,
        .order_type = order.type,
        .time_in_force = order.time_in_force,
        .quantity = order.quantity,
        .price_text = std::string(order.price_text),
        .reduce_only = order.reduce_only,
    });
    return {.status = next_place_status};
  }

  SendResult CancelOrder(aquila::core::StrategyOrder&) noexcept {
    return {};
  }

  SendStatus next_place_status{SendStatus::kOk};
  std::vector<CapturedOrder> placed_orders;
};

using OrderManagerT = aquila::core::OrderManager<FakeOrderSession>;
using ContextT = aquila::core::StrategyContext<FakeOrderSession>;

leadlag::Config OnePairConfig() {
  leadlag::Config config;
  config.name = "lead_lag";
  config.version = "1.0";
  config.pairs.push_back(leadlag::PairConfig{
      .symbol = "BTC_USDT",
      .symbol_id = 3,
      .lead_exchange = aquila::Exchange::kBinance,
      .lag_exchange = aquila::Exchange::kGate,
  });
  return config;
}

leadlag::Config SignalOnlyConfig() {
  leadlag::Config config;
  config.name = "lead_lag";
  config.version = "1.0";
  config.pairs.push_back(leadlag::PairConfig{
      .symbol = "BTC_USDT",
      .symbol_id = 3,
      .lead_exchange = aquila::Exchange::kBinance,
      .lag_exchange = aquila::Exchange::kGate,
      .lag_taker_fee = 0.0,
      .trigger =
          leadlag::TriggerConfig{
              .lead = 0.02,
              .close = 0.005,
              .lag_part = 0.5,
              .target_profit_rate = 0.0,
              .drift_limit = 1.0,
              .drift_period_ns = 1'000'000'000,
              .drift_min_samples = 1,
              .drift_warmup_ns = 1,
              .quantile =
                  leadlag::QuantileConfig{
                      .move = 0.75,
                      .up_min = 0.0,
                      .up_max = 0.10,
                      .down_min = -0.10,
                      .down_max = 0.0,
                      .precision = 0.000001,
                  },
          },
      .execute =
          leadlag::ExecuteConfig{
              .open_notional = 1000.0,
              .trailing_stop = 0.05,
              .max_entry_spread = 0.02,
              .parallel = 1,
          },
      .bbo_record =
          leadlag::BboRecordConfig{
              .window_ns = 1'000'000'000,
              .stats_window_ns = 1'000'000'000,
          },
      .lag_instrument =
          leadlag::InstrumentMetadata{
              .exchange = aquila::Exchange::kGate,
              .exchange_symbol = "BTC_USDT_GATE",
              .price_tick = 0.1,
              .price_decimal_places = 1,
              .quantity_step = 1.0,
              .min_quantity = 1.0,
              .max_quantity = 20.0,
              .notional_multiplier = 1.0,
          },
  });
  return config;
}

aquila::config::StrategyConfig RuntimeConfig() {
  aquila::config::StrategyConfig config;
  config.name = "lead_lag";
  config.strategy_id = 4;
  config.order_capacity = 8;
  config.feedback.enabled = false;
  return config;
}

aquila::BookTicker Ticker(std::int32_t symbol_id, aquila::Exchange exchange,
                          std::int64_t local_ns, double bid_price,
                          double ask_price) {
  return aquila::BookTicker{
      .id = local_ns,
      .symbol_id = symbol_id,
      .exchange = exchange,
      .exchange_ns = local_ns - 10,
      .local_ns = local_ns,
      .bid_price = bid_price,
      .bid_volume = 1.0,
      .ask_price = ask_price,
      .ask_volume = 1.0,
  };
}

void FeedOpenLongSignal(leadlag::Strategy* strategy, ContextT* context) {
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kGate, 100, 101.57, 102.02), *context);
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), *context);
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), *context);
}

aquila::OrderFeedbackEvent FilledFeedback(std::uint64_t local_order_id,
                                          std::int64_t quantity,
                                          double fill_price) {
  return aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kFilled,
      .local_order_id = local_order_id,
      .exchange_order_id = local_order_id + 1000,
      .cumulative_filled_quantity = quantity,
      .left_quantity = 0,
      .cancelled_quantity = 0,
      .fill_price = fill_price,
      .role = aquila::OrderRole::kTaker,
      .finish_reason = aquila::OrderFinishReason::kImmediateOrCancel,
      .reject_reason = aquila::OrderRejectReason::kUnknown,
      .continuity_scope = aquila::OrderFeedbackContinuityScope::kLane,
      .continuity_reason = aquila::OrderFeedbackContinuityReason::kUnknown,
      .continuity_sequence = 0,
      .exchange_update_ns = 200,
      .local_receive_ns = 201,
  };
}

void ApplyFeedback(leadlag::Strategy* strategy, OrderManagerT* order_manager,
                   ContextT* context, const aquila::OrderFeedbackEvent& event) {
  order_manager->OnOrderFeedback(event);
  strategy->OnOrderFeedback(event, *context);
}

TEST(LeadLagStrategyInterfaceTest, RuntimeCanDispatchHooks) {
  using Runtime =
      aquila::core::TradingRuntime<leadlag::Strategy, FakeOrderSession>;

  auto runtime_result = Runtime::CreateForTest(
      RuntimeConfig(), [] { return FakeOrderSession{}; }, OnePairConfig());

  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  runtime_result.value->HandleBookTickerForTest(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0));
  runtime_result.value->HandleBookTickerForTest(
      Ticker(3, aquila::Exchange::kGate, 110, 99.5, 100.5));
  runtime_result.value->HandleOrderResponseForTest(
      aquila::core::OrderResponseEvent{
          .kind = aquila::core::OrderResponseKind::kAck,
          .local_order_id = 0,
      });
  runtime_result.value->HandleOrderFeedbackForTest(aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kContinuityLost,
      .continuity_scope = aquila::OrderFeedbackContinuityScope::kLane,
      .continuity_reason =
          aquila::OrderFeedbackContinuityReason::kSessionDisconnected,
  });
}

TEST(LeadLagStrategyInterfaceTest, StoresRawMarketUpdates) {
  leadlag::Strategy strategy{OnePairConfig()};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 110, 99.5, 100.5),
                        context);

  EXPECT_EQ(strategy.last_market_update().role, leadlag::PairRole::kLag);
  EXPECT_TRUE(strategy.last_market_update().both_sides_valid);
  ASSERT_NE(strategy.raw_market_state().FindPair(3), nullptr);

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kContinuityLost,
      },
      context);
  EXPECT_TRUE(strategy.degraded());
  EXPECT_FALSE(strategy.ShouldStop());
}

TEST(LeadLagStrategyInterfaceTest, LeadTickEmitsOpenSignalAfterAlignment) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kOpenLong);
  EXPECT_NE(decision.group_id, 0U);
  EXPECT_DOUBLE_EQ(decision.trailing_price, 0.0);
  EXPECT_EQ(decision.intent.side, aquila::OrderSide::kBuy);
  EXPECT_FALSE(decision.intent.reduce_only);
  EXPECT_EQ(decision.intent.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(decision.intent.symbol_id, 3);
  EXPECT_TRUE(strategy.last_signal_diagnostics_valid());

  const leadlag::SignalDiagnostics& diagnostics =
      strategy.last_signal_diagnostics();
  EXPECT_EQ(diagnostics.role, leadlag::PairRole::kLead);
  EXPECT_TRUE(diagnostics.price_changed);
  EXPECT_EQ(diagnostics.event_ns, 91);
  EXPECT_EQ(diagnostics.lead_raw.event_ns, 91);
  EXPECT_DOUBLE_EQ(diagnostics.lead_raw.bid_price, 112.0);
  EXPECT_DOUBLE_EQ(diagnostics.lead_raw.ask_price, 113.0);
  EXPECT_EQ(diagnostics.lag.event_ns, 90);
  EXPECT_DOUBLE_EQ(diagnostics.lag.bid_price, 101.5);
  EXPECT_DOUBLE_EQ(diagnostics.lag.ask_price, 102.0);
  EXPECT_EQ(diagnostics.position_direction, leadlag::PositionDirection::kLong);
  EXPECT_EQ(diagnostics.active_group_count, 1U);
  EXPECT_EQ(diagnostics.group_id, decision.group_id);
  EXPECT_DOUBLE_EQ(diagnostics.trailing_price, 0.0);
}

TEST(LeadLagStrategyInterfaceTest, DefaultModeDoesNotCreateSyntheticHold) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);
  ASSERT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  EXPECT_FALSE(decision.triggered);
  EXPECT_NE(decision.action, leadlag::SignalAction::kCloseLong);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModePlacesIocLimitOrderOnOpenSignal) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const FakeOrderSession::CapturedOrder& order =
      order_session.placed_orders.back();
  EXPECT_EQ(order.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(order.symbol_id, 3);
  EXPECT_EQ(order.symbol, "BTC_USDT_GATE");
  EXPECT_EQ(order.side, aquila::OrderSide::kBuy);
  EXPECT_EQ(order.order_type, aquila::OrderType::kLimit);
  EXPECT_EQ(order.time_in_force, aquila::TimeInForce::kImmediateOrCancel);
  EXPECT_FALSE(order.reduce_only);
  EXPECT_EQ(order.price_text, "102.1");
  EXPECT_EQ(order.quantity, 9);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kOpenLong);
  EXPECT_NE(decision.group_id, 0U);
  ASSERT_TRUE(strategy.last_signal_diagnostics_valid());
  EXPECT_EQ(strategy.last_signal_diagnostics().group_id, decision.group_id);
  EXPECT_EQ(strategy.last_signal_diagnostics().active_group_count, 1U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeUsesFilledPositionQuantityForCloseOrder) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));
  EXPECT_EQ(context.FindOrder(open_order_id), nullptr);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const FakeOrderSession::CapturedOrder& close_order =
      order_session.placed_orders.back();
  EXPECT_EQ(close_order.side, aquila::OrderSide::kSell);
  EXPECT_EQ(close_order.order_type, aquila::OrderType::kLimit);
  EXPECT_EQ(close_order.time_in_force, aquila::TimeInForce::kImmediateOrCancel);
  EXPECT_TRUE(close_order.reduce_only);
  EXPECT_EQ(close_order.price_text, "101.5");
  EXPECT_EQ(close_order.quantity, 7);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kCloseLong);
  EXPECT_NE(decision.group_id, 0U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeUsesFilledPositionQuantityForStoplossOrder) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 11, 102.1));

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 95.0, 96.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const FakeOrderSession::CapturedOrder& stoploss_order =
      order_session.placed_orders.back();
  EXPECT_EQ(stoploss_order.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(stoploss_order.reduce_only);
  EXPECT_EQ(stoploss_order.price_text, "94.5");
  EXPECT_EQ(stoploss_order.quantity, 11);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kStoplossLong);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRollsBackSessionRejectedOpenOrder) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  order_session.next_place_status = FakeOrderSession::SendStatus::kRejected;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t rejected_order_id =
      order_session.placed_orders.back().local_order_id;
  EXPECT_EQ(context.FindOrder(rejected_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  order_session.next_place_status = FakeOrderSession::SendStatus::kOk;
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 102, 80.0, 81.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  EXPECT_NE(order_session.placed_orders.back().local_order_id,
            rejected_order_id);
  EXPECT_TRUE(strategy.last_signal_decision().triggered);
  EXPECT_NE(strategy.last_signal_decision().group_id, 0U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRollsBackResponseRejectedOpenOrder) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t rejected_order_id =
      order_session.placed_orders.back().local_order_id;

  const aquila::core::OrderResponseEvent rejected{
      .kind = aquila::core::OrderResponseKind::kRejected,
      .local_order_id = rejected_order_id,
  };
  order_manager.OnOrderResponse(rejected);
  strategy.OnOrderResponse(rejected, context);

  EXPECT_EQ(context.FindOrder(rejected_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 102, 80.0, 81.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  EXPECT_NE(order_session.placed_orders.back().local_order_id,
            rejected_order_id);
  EXPECT_TRUE(strategy.last_signal_decision().triggered);
  EXPECT_NE(strategy.last_signal_decision().group_id, 0U);
}

TEST(LeadLagStrategyInterfaceTest, ReplayModeEmitsCloseSignalForSyntheticHold) {
  leadlag::Strategy strategy{
      SignalOnlyConfig(),
      leadlag::StrategyOptions{
          .position_accounting =
              leadlag::PositionAccountingMode::kSyntheticSignals,
      }};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);
  ASSERT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kCloseLong);
  EXPECT_NE(decision.group_id, 0U);
  EXPECT_DOUBLE_EQ(decision.trailing_price, 102.0);
  EXPECT_EQ(decision.intent.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(decision.intent.reduce_only);

  EXPECT_TRUE(strategy.last_signal_diagnostics_valid());
  const leadlag::SignalDiagnostics& diagnostics =
      strategy.last_signal_diagnostics();
  EXPECT_EQ(diagnostics.group_id, decision.group_id);
  EXPECT_DOUBLE_EQ(diagnostics.trailing_price, 102.0);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  EXPECT_FALSE(strategy.last_signal_diagnostics_valid());
}

TEST(LeadLagStrategyInterfaceTest,
     ReplayModeClearsTriggeredSyntheticGroupById) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].execute.parallel = 2;
  leadlag::Strategy strategy{
      config, leadlag::StrategyOptions{
                  .position_accounting =
                      leadlag::PositionAccountingMode::kSyntheticSignals,
              }};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);
  ASSERT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 105.0, 106.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 103, 170.0, 171.0), context);
  ASSERT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 104, 100.4, 101.4),
                        context);

  const leadlag::SignalDecision& stoploss = strategy.last_signal_decision();
  ASSERT_TRUE(stoploss.triggered);
  ASSERT_EQ(stoploss.action, leadlag::SignalAction::kStoplossLong);
  ASSERT_NE(stoploss.group_id, 0U);
  const std::uint64_t stopped_group_id = stoploss.group_id;
  EXPECT_DOUBLE_EQ(stoploss.trailing_price, 106.0);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 106, 100.0, 101.0), context);

  const leadlag::SignalDecision& close = strategy.last_signal_decision();
  ASSERT_TRUE(close.triggered) << static_cast<int>(close.reject_reason);
  ASSERT_EQ(close.action, leadlag::SignalAction::kCloseLong);
  EXPECT_NE(close.group_id, stopped_group_id);
}

TEST(LeadLagStrategyInterfaceTest, FeedbackContinuityLostPausesNewOpenSignals) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kContinuityLost,
      },
      context);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  EXPECT_FALSE(decision.triggered);
  EXPECT_EQ(decision.reject_reason, leadlag::SignalRejectReason::kDegraded);
  EXPECT_TRUE(strategy.degraded());
}

}  // namespace
