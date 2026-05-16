#include <cstdint>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/config/strategy_config.h"
#include "core/market_data/types.h"
#include "core/strategy/order_types.h"
#include "core/strategy/strategy_runtime.h"
#include "core/trading/order_feedback_event.h"
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

  SendResult PlaceOrder(aquila::strategy::StrategyOrder&) noexcept {
    return {};
  }

  SendResult CancelOrder(aquila::strategy::StrategyOrder&) noexcept {
    return {};
  }
};

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
              .open_notional = 100.0,
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
              .exchange_symbol = "BTC_USDT",
              .quantity_step = 1.0,
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

TEST(LeadLagStrategyInterfaceTest, RuntimeCanDispatchHooks) {
  using Runtime =
      aquila::strategy::StrategyRuntime<leadlag::Strategy, FakeOrderSession>;

  auto runtime_result = Runtime::CreateForTest(
      RuntimeConfig(), [] { return FakeOrderSession{}; }, OnePairConfig());

  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  runtime_result.value->HandleBookTickerForTest(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0));
  runtime_result.value->HandleBookTickerForTest(
      Ticker(3, aquila::Exchange::kGate, 110, 99.5, 100.5));
  runtime_result.value->HandleOrderResponseForTest(
      aquila::strategy::OrderResponseEvent{
          .kind = aquila::strategy::OrderResponseKind::kAck,
          .local_order_id = 0,
      });
  runtime_result.value->HandleOrderFeedbackForTest(aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kGap,
      .gap_scope = aquila::OrderFeedbackGapScope::kLane,
      .gap_reason = aquila::OrderFeedbackGapReason::kSessionDisconnected,
  });
}

TEST(LeadLagStrategyInterfaceTest, StoresRawMarketUpdates) {
  leadlag::Strategy strategy{OnePairConfig()};
  FakeOrderSession order_session;
  aquila::strategy::OrderManager<FakeOrderSession> order_manager{order_session,
                                                                 8, 4};
  aquila::strategy::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 110, 99.5, 100.5),
                        context);

  EXPECT_EQ(strategy.last_market_update().role, leadlag::PairRole::kLag);
  EXPECT_TRUE(strategy.last_market_update().both_sides_valid);
  ASSERT_NE(strategy.raw_market_state().FindPair(3), nullptr);

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kGap,
      },
      context);
  EXPECT_TRUE(strategy.degraded());
  EXPECT_FALSE(strategy.ShouldStop());
}

TEST(LeadLagStrategyInterfaceTest, LeadTickEmitsOpenSignalAfterAlignment) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  aquila::strategy::OrderManager<FakeOrderSession> order_manager{order_session,
                                                                 8, 4};
  aquila::strategy::StrategyContext<FakeOrderSession> context{order_manager};

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
  EXPECT_EQ(decision.intent.side, aquila::OrderSide::kBuy);
  EXPECT_FALSE(decision.intent.reduce_only);
  EXPECT_EQ(decision.intent.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(decision.intent.symbol_id, 3);

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
  EXPECT_EQ(diagnostics.position_direction,
            leadlag::PositionDirection::kLong);
  EXPECT_EQ(diagnostics.active_group_count, 0U);
  EXPECT_DOUBLE_EQ(diagnostics.trailing_price, 0.0);
}

TEST(LeadLagStrategyInterfaceTest, DefaultModeDoesNotCreateSyntheticHold) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  aquila::strategy::OrderManager<FakeOrderSession> order_manager{order_session,
                                                                 8, 4};
  aquila::strategy::StrategyContext<FakeOrderSession> context{order_manager};

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

TEST(LeadLagStrategyInterfaceTest, ReplayModeEmitsCloseSignalForSyntheticHold) {
  leadlag::Strategy strategy{
      SignalOnlyConfig(),
      leadlag::StrategyOptions{
          .position_accounting =
              leadlag::PositionAccountingMode::kSyntheticSignals,
      }};
  FakeOrderSession order_session;
  aquila::strategy::OrderManager<FakeOrderSession> order_manager{order_session,
                                                                 8, 4};
  aquila::strategy::StrategyContext<FakeOrderSession> context{order_manager};

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
  EXPECT_EQ(decision.intent.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(decision.intent.reduce_only);
}

TEST(LeadLagStrategyInterfaceTest, FeedbackGapPausesNewOpenSignals) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  aquila::strategy::OrderManager<FakeOrderSession> order_manager{order_session,
                                                                 8, 4};
  aquila::strategy::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kGap,
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
