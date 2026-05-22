#include "tools/lead_lag/live_strategy.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/config/strategy_config.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/execution_state.h"

namespace aquila::tools::lead_lag {
namespace {

namespace leadlag = aquila::strategy::leadlag;

struct TestStrategyContext {
  [[nodiscard]] const aquila::core::StrategyOrder* FindOrder(
      std::uint64_t) const noexcept {
    return nullptr;
  }

  bool RetireFinishedOrder(std::uint64_t) noexcept {
    return false;
  }
};

struct CapturedOrder {
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

struct SmokeStrategyContext {
  aquila::core::OrderPlaceStatus place_status{
      aquila::core::OrderPlaceStatus::kOk};
  aquila::core::OrderCancelStatus cancel_status{
      aquila::core::OrderCancelStatus::kOk};
  std::uint64_t next_local_order_id{100};
  std::vector<CapturedOrder> orders;
  std::vector<std::uint64_t> cancelled_local_order_ids;

  aquila::core::OrderPlaceResult PlaceOrder(
      aquila::core::OrderCreateRequest request) {
    orders.push_back(CapturedOrder{
        .exchange = request.exchange,
        .symbol_id = request.symbol_id,
        .symbol = std::string(request.symbol),
        .side = request.side,
        .order_type = request.order_type,
        .time_in_force = request.time_in_force,
        .quantity = request.quantity,
        .price_text = std::string(request.price_text),
        .reduce_only = request.reduce_only,
    });
    if (place_status != aquila::core::OrderPlaceStatus::kOk) {
      return {.status = place_status, .local_order_id = 0};
    }
    return {.status = place_status, .local_order_id = next_local_order_id++};
  }

  aquila::core::OrderCancelResult CancelOrder(std::uint64_t local_order_id) {
    cancelled_local_order_ids.push_back(local_order_id);
    if (cancel_status != aquila::core::OrderCancelStatus::kOk) {
      return {.status = cancel_status, .local_order_id = local_order_id};
    }
    return {.status = cancel_status, .local_order_id = local_order_id};
  }
};

leadlag::Config MakeLeadLagConfig() {
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

TEST(LeadLagLiveStrategyTest, DefaultsToValidateOnlyWithoutLiveFlags) {
  const RunModeResult result = ResolveRunMode(
      aquila::config::StrategyMode::kDryRun, /*connect_data=*/false,
      /*execute=*/false);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.mode, RunMode::kValidateOnly);
}

TEST(LeadLagLiveStrategyTest, ConnectDataSelectsSignalOnlyWithoutExecute) {
  const RunModeResult result = ResolveRunMode(
      aquila::config::StrategyMode::kDryRun, /*connect_data=*/true,
      /*execute=*/false);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.mode, RunMode::kSignalOnly);
}

TEST(LeadLagLiveStrategyTest,
     ConnectDataSelectsSignalOnlyWithLiveStrategyModeWithoutExecute) {
  const RunModeResult result =
      ResolveRunMode(aquila::config::StrategyMode::kLive, /*connect_data=*/true,
                     /*execute=*/false);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.mode, RunMode::kSignalOnly);
}

TEST(LeadLagLiveStrategyTest, ExecuteRequiresLiveStrategyMode) {
  const RunModeResult result = ResolveRunMode(
      aquila::config::StrategyMode::kDryRun, /*connect_data=*/false,
      /*execute=*/true);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("strategy.mode must be live"), std::string::npos);
}

TEST(LeadLagLiveStrategyTest, ExecuteSelectsLiveOrdersWithLiveMode) {
  const RunModeResult result = ResolveRunMode(
      aquila::config::StrategyMode::kLive, /*connect_data=*/false,
      /*execute=*/true);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.mode, RunMode::kLiveOrders);
}

TEST(LeadLagLiveStrategyTest, SmokeOpenCloseRequiresExecute) {
  const RunModeResult result =
      ResolveRunMode(aquila::config::StrategyMode::kLive, /*connect_data=*/true,
                     /*execute=*/false, /*smoke_open_close=*/true);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("--smoke-open-close requires --execute"),
            std::string::npos);
}

TEST(LeadLagLiveStrategyTest, SmokeOpenCloseSelectsDedicatedRunMode) {
  const RunModeResult result =
      ResolveRunMode(aquila::config::StrategyMode::kLive, /*connect_data=*/true,
                     /*execute=*/true, /*smoke_open_close=*/true);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.mode, RunMode::kLiveOpenCloseSmoke);
}

TEST(LeadLagLiveStrategyTest, SmokeUnfilledCancelDefaultPassiveOffsetIsSafe) {
  EXPECT_DOUBLE_EQ(LiveUnfilledCancelSmokeOptions{}.passive_price_bps, 200.0);
}

TEST(LeadLagLiveStrategyTest, SmokeUnfilledCancelRequiresExecute) {
  const RunModeResult result =
      ResolveRunMode(aquila::config::StrategyMode::kLive, /*connect_data=*/true,
                     /*execute=*/false, /*smoke_open_close=*/false,
                     /*smoke_unfilled_cancel=*/true);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("--smoke-unfilled-cancel requires --execute"),
            std::string::npos);
}

TEST(LeadLagLiveStrategyTest, SmokeModesAreMutuallyExclusive) {
  const RunModeResult result =
      ResolveRunMode(aquila::config::StrategyMode::kLive, /*connect_data=*/true,
                     /*execute=*/true, /*smoke_open_close=*/true,
                     /*smoke_unfilled_cancel=*/true);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("only one smoke mode may be selected"),
            std::string::npos);
}

TEST(LeadLagLiveStrategyTest, SmokeUnfilledCancelSelectsDedicatedRunMode) {
  const RunModeResult result =
      ResolveRunMode(aquila::config::StrategyMode::kLive, /*connect_data=*/true,
                     /*execute=*/true, /*smoke_open_close=*/false,
                     /*smoke_unfilled_cancel=*/true);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.mode, RunMode::kLiveUnfilledCancelSmoke);
}

TEST(LeadLagLiveStrategyTest, ExecuteTakesPriorityOverConnectDataWithLiveMode) {
  const RunModeResult result =
      ResolveRunMode(aquila::config::StrategyMode::kLive, /*connect_data=*/true,
                     /*execute=*/true);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.mode, RunMode::kLiveOrders);
}

TEST(LeadLagLiveStrategyTest, RunModeNameReturnsStableSummaryText) {
  EXPECT_STREQ(RunModeName(RunMode::kValidateOnly), "validate_only");
  EXPECT_STREQ(RunModeName(RunMode::kSignalOnly), "signal_only");
  EXPECT_STREQ(RunModeName(RunMode::kLiveOrders), "live_orders");
  EXPECT_STREQ(RunModeName(RunMode::kLiveOpenCloseSmoke),
               "live_open_close_smoke");
  EXPECT_STREQ(RunModeName(RunMode::kLiveUnfilledCancelSmoke),
               "live_unfilled_cancel_smoke");
}

TEST(LeadLagLiveStrategyTest, RecoveryStateNameReturnsStableSummaryText) {
  namespace leadlag = aquila::strategy::leadlag;

  EXPECT_STREQ(RecoveryStateName(leadlag::RecoveryState::kNormal), "normal");
  EXPECT_STREQ(
      RecoveryStateName(leadlag::RecoveryState::kDegradedNeedsReconcile),
      "degraded_needs_reconcile");
  EXPECT_STREQ(RecoveryStateName(leadlag::RecoveryState::kReconciling),
               "reconciling");
  EXPECT_STREQ(RecoveryStateName(leadlag::RecoveryState::kRecovered),
               "recovered");
  EXPECT_STREQ(RecoveryStateName(leadlag::RecoveryState::kManualIntervention),
               "manual_intervention");
}

TEST(LeadLagLiveStrategyTest, FormatsRecoveryDiagnosticsSummaryFields) {
  namespace leadlag = aquila::strategy::leadlag;

  const RecoveryDiagnostics diagnostics{
      .recovery_state = leadlag::RecoveryState::kDegradedNeedsReconcile,
      .needs_reconcile = true,
      .manual_intervention = false,
      .new_entries_paused = true,
  };

  const std::string fields = FormatRecoveryDiagnosticsFields(diagnostics);

  EXPECT_NE(fields.find("recovery_state=degraded_needs_reconcile"),
            std::string::npos);
  EXPECT_NE(fields.find("needs_reconcile=true"), std::string::npos);
  EXPECT_NE(fields.find("manual_intervention=false"), std::string::npos);
  EXPECT_NE(fields.find("new_entries_paused=true"), std::string::npos);
}

TEST(LeadLagLiveStrategyTest,
     LiveOrdersStrategyStopsAndReportsDiagnosticsOnContinuityLost) {
  LiveOrdersStrategy strategy{MakeLeadLagConfig()};
  TestStrategyContext context;

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kContinuityLost,
          .continuity_scope = aquila::OrderFeedbackContinuityScope::kGlobal,
          .continuity_reason =
              aquila::OrderFeedbackContinuityReason::kReconnectUnknownWindow,
          .continuity_sequence = 42,
      },
      context);

  EXPECT_TRUE(strategy.ShouldStop());
  EXPECT_TRUE(strategy.stats().continuity_lost_stop_requested);
  EXPECT_EQ(strategy.stats().recovery.recovery_state,
            leadlag::RecoveryState::kDegradedNeedsReconcile);
  EXPECT_TRUE(strategy.stats().recovery.needs_reconcile);
  EXPECT_FALSE(strategy.stats().recovery.manual_intervention);
  EXPECT_TRUE(strategy.stats().recovery.new_entries_paused);
}

TEST(LeadLagLiveStrategyTest,
     LiveOrdersStrategyIgnoresNonContinuityLostFeedbackForEmergencyStop) {
  LiveOrdersStrategy strategy{MakeLeadLagConfig()};
  TestStrategyContext context;

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kAccepted,
      },
      context);

  EXPECT_FALSE(strategy.ShouldStop());
  EXPECT_FALSE(strategy.stats().continuity_lost_stop_requested);
  EXPECT_EQ(strategy.stats().recovery.recovery_state,
            leadlag::RecoveryState::kNormal);
  EXPECT_FALSE(strategy.stats().recovery.needs_reconcile);
  EXPECT_FALSE(strategy.stats().recovery.new_entries_paused);
}

TEST(LeadLagLiveStrategyTest,
     ResolveLiveOrdersExitCodePrioritizesContinuityLostHandoff) {
  LiveOrdersStrategyStats stats;
  stats.continuity_lost_stop_requested = true;

  EXPECT_EQ(ResolveLiveOrdersExitCode(/*runtime_exit_code=*/0, stats),
            kContinuityLostEmergencyHandoffExitCode);
  EXPECT_EQ(ResolveLiveOrdersExitCode(/*runtime_exit_code=*/1, stats),
            kContinuityLostEmergencyHandoffExitCode);
}

TEST(LeadLagLiveStrategyTest,
     ResolveLiveOrdersExitCodePreservesRuntimeCodeWithoutContinuityLost) {
  LiveOrdersStrategyStats stats;

  EXPECT_EQ(ResolveLiveOrdersExitCode(/*runtime_exit_code=*/0, stats), 0);
  EXPECT_EQ(ResolveLiveOrdersExitCode(/*runtime_exit_code=*/7, stats), 7);
}

TEST(LeadLagLiveStrategyTest,
     SmokeOpenCloseSubmitsAggressiveOpenThenReduceOnlyAggressiveClose) {
  LiveOpenCloseSmokeStats stats;
  LiveOpenCloseSmokeStrategy strategy{
      MakeLeadLagConfig(),
      LiveOpenCloseSmokeOptions{
          .symbol = "BTC_USDT",
          .aggressive_price_bps = 100.0,
          .max_notional = 2000.0,
      },
      &stats,
  };
  SmokeStrategyContext context;

  strategy.OnBookTicker(
      aquila::BookTicker{
          .symbol_id = 3,
          .exchange = aquila::Exchange::kGate,
          .bid_price = 99.0,
          .ask_price = 100.0,
      },
      context);

  ASSERT_EQ(context.orders.size(), 1U);
  EXPECT_EQ(context.orders[0].exchange, aquila::Exchange::kGate);
  EXPECT_EQ(context.orders[0].symbol_id, 3);
  EXPECT_EQ(context.orders[0].symbol, "BTC_USDT_GATE");
  EXPECT_EQ(context.orders[0].side, aquila::OrderSide::kBuy);
  EXPECT_EQ(context.orders[0].time_in_force,
            aquila::TimeInForce::kImmediateOrCancel);
  EXPECT_EQ(context.orders[0].quantity, 10);
  EXPECT_EQ(context.orders[0].price_text, "101.0");
  EXPECT_FALSE(context.orders[0].reduce_only);
  EXPECT_EQ(stats.state, LiveOpenCloseSmokeState::kOpenPending);

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kFilled,
          .local_order_id = stats.open_local_order_id,
          .cumulative_filled_quantity = 10,
      },
      context);

  EXPECT_EQ(context.orders.size(), 1U);
  EXPECT_EQ(stats.state, LiveOpenCloseSmokeState::kWaitingCloseTicker);

  strategy.OnBookTicker(
      aquila::BookTicker{
          .symbol_id = 3,
          .exchange = aquila::Exchange::kGate,
          .bid_price = 99.0,
          .ask_price = 100.0,
      },
      context);

  ASSERT_EQ(context.orders.size(), 2U);
  EXPECT_EQ(context.orders[1].side, aquila::OrderSide::kSell);
  EXPECT_EQ(context.orders[1].time_in_force,
            aquila::TimeInForce::kImmediateOrCancel);
  EXPECT_EQ(context.orders[1].quantity, 10);
  EXPECT_EQ(context.orders[1].price_text, "98.0");
  EXPECT_TRUE(context.orders[1].reduce_only);
  EXPECT_EQ(stats.state, LiveOpenCloseSmokeState::kClosePending);

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kFilled,
          .local_order_id = stats.close_local_order_id,
          .cumulative_filled_quantity = 10,
      },
      context);

  EXPECT_TRUE(strategy.ShouldStop());
  EXPECT_EQ(stats.state, LiveOpenCloseSmokeState::kDone);
  EXPECT_TRUE(stats.completed);
}

TEST(LeadLagLiveStrategyTest,
     SmokeOpenCloseUsesMinQuantityWhenTargetNotionalIsBelowMinimum) {
  leadlag::Config config = MakeLeadLagConfig();
  config.pairs[0].execute.open_notional = 10.0;
  LiveOpenCloseSmokeStats stats;
  LiveOpenCloseSmokeStrategy strategy{
      std::move(config),
      LiveOpenCloseSmokeOptions{
          .symbol = "BTC_USDT",
          .aggressive_price_bps = 0.0,
          .max_notional = 100.0,
      },
      &stats,
  };
  SmokeStrategyContext context;

  strategy.OnBookTicker(
      aquila::BookTicker{
          .symbol_id = 3,
          .exchange = aquila::Exchange::kGate,
          .bid_price = 39.0,
          .ask_price = 40.0,
      },
      context);

  ASSERT_EQ(context.orders.size(), 1U);
  EXPECT_EQ(context.orders[0].quantity, 1);
  EXPECT_TRUE(stats.used_min_quantity);
  EXPECT_DOUBLE_EQ(stats.target_notional, 10.0);
  EXPECT_DOUBLE_EQ(stats.estimated_open_notional, 40.0);
}

TEST(LeadLagLiveStrategyTest,
     SmokeOpenCloseRejectsMinimumQuantityAboveNotionalCap) {
  leadlag::Config config = MakeLeadLagConfig();
  config.pairs[0].execute.open_notional = 10.0;
  LiveOpenCloseSmokeStats stats;
  LiveOpenCloseSmokeStrategy strategy{
      std::move(config),
      LiveOpenCloseSmokeOptions{
          .symbol = "BTC_USDT",
          .aggressive_price_bps = 0.0,
          .max_notional = 30.0,
      },
      &stats,
  };
  SmokeStrategyContext context;

  strategy.OnBookTicker(
      aquila::BookTicker{
          .symbol_id = 3,
          .exchange = aquila::Exchange::kGate,
          .bid_price = 39.0,
          .ask_price = 40.0,
      },
      context);

  EXPECT_TRUE(context.orders.empty());
  EXPECT_TRUE(strategy.ShouldStop());
  EXPECT_EQ(stats.state, LiveOpenCloseSmokeState::kError);
  EXPECT_NE(stats.error.find("minimum notional exceeds cap"),
            std::string::npos);
}

TEST(LeadLagLiveStrategyTest,
     SmokeUnfilledCancelSubmitsPassiveGtcBuyThenCancelsOnAcceptedResponse) {
  LiveUnfilledCancelSmokeStats stats;
  LiveUnfilledCancelSmokeStrategy strategy{
      MakeLeadLagConfig(),
      LiveUnfilledCancelSmokeOptions{
          .symbol = "BTC_USDT",
          .passive_price_bps = 500.0,
          .max_notional = 2000.0,
      },
      &stats,
  };
  SmokeStrategyContext context;

  strategy.OnBookTicker(
      aquila::BookTicker{
          .symbol_id = 3,
          .exchange = aquila::Exchange::kGate,
          .bid_price = 100.0,
          .ask_price = 101.0,
      },
      context);

  ASSERT_EQ(context.orders.size(), 1U);
  EXPECT_EQ(context.orders[0].exchange, aquila::Exchange::kGate);
  EXPECT_EQ(context.orders[0].symbol_id, 3);
  EXPECT_EQ(context.orders[0].symbol, "BTC_USDT_GATE");
  EXPECT_EQ(context.orders[0].side, aquila::OrderSide::kBuy);
  EXPECT_EQ(context.orders[0].time_in_force,
            aquila::TimeInForce::kGoodTillCancel);
  EXPECT_EQ(context.orders[0].quantity, 10);
  EXPECT_EQ(context.orders[0].price_text, "95.0");
  EXPECT_FALSE(context.orders[0].reduce_only);
  EXPECT_EQ(stats.state, LiveUnfilledCancelSmokeState::kOpenPending);

  strategy.OnOrderResponse(
      aquila::core::OrderResponseEvent{
          .kind = aquila::core::OrderResponseKind::kAccepted,
          .local_order_id = stats.open_local_order_id,
          .exchange_order_id = 9001,
      },
      context);

  ASSERT_EQ(context.cancelled_local_order_ids.size(), 1U);
  EXPECT_EQ(context.cancelled_local_order_ids[0], stats.open_local_order_id);
  EXPECT_TRUE(stats.cancel_requested);
  EXPECT_EQ(stats.state, LiveUnfilledCancelSmokeState::kCancelPending);

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kCancelled,
          .local_order_id = stats.open_local_order_id,
          .cumulative_filled_quantity = 0,
          .cancelled_quantity = 10,
      },
      context);

  EXPECT_TRUE(strategy.ShouldStop());
  EXPECT_TRUE(stats.completed);
  EXPECT_EQ(stats.state, LiveUnfilledCancelSmokeState::kDone);
}

TEST(LeadLagLiveStrategyTest,
     SmokeUnfilledCancelCanCancelOnAcceptedFeedbackWhenResponseIsNotObserved) {
  LiveUnfilledCancelSmokeStats stats;
  LiveUnfilledCancelSmokeStrategy strategy{
      MakeLeadLagConfig(),
      LiveUnfilledCancelSmokeOptions{
          .symbol = "BTC_USDT",
          .passive_price_bps = 500.0,
          .max_notional = 2000.0,
      },
      &stats,
  };
  SmokeStrategyContext context;

  strategy.OnBookTicker(
      aquila::BookTicker{
          .symbol_id = 3,
          .exchange = aquila::Exchange::kGate,
          .bid_price = 100.0,
          .ask_price = 101.0,
      },
      context);
  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kAccepted,
          .local_order_id = stats.open_local_order_id,
      },
      context);

  ASSERT_EQ(context.cancelled_local_order_ids.size(), 1U);
  EXPECT_EQ(context.cancelled_local_order_ids[0], stats.open_local_order_id);
  EXPECT_TRUE(stats.cancel_requested);
  EXPECT_EQ(stats.state, LiveUnfilledCancelSmokeState::kCancelPending);
}

TEST(LeadLagLiveStrategyTest,
     SmokeUnfilledCancelDoesNotCompleteOnCancelAcceptedResponse) {
  LiveUnfilledCancelSmokeStats stats;
  LiveUnfilledCancelSmokeStrategy strategy{
      MakeLeadLagConfig(),
      LiveUnfilledCancelSmokeOptions{
          .symbol = "BTC_USDT",
          .passive_price_bps = 500.0,
          .max_notional = 2000.0,
      },
      &stats,
  };
  SmokeStrategyContext context;

  strategy.OnBookTicker(
      aquila::BookTicker{
          .symbol_id = 3,
          .exchange = aquila::Exchange::kGate,
          .bid_price = 100.0,
          .ask_price = 101.0,
      },
      context);
  strategy.OnOrderResponse(
      aquila::core::OrderResponseEvent{
          .kind = aquila::core::OrderResponseKind::kAccepted,
          .local_order_id = stats.open_local_order_id,
      },
      context);
  strategy.OnOrderResponse(
      aquila::core::OrderResponseEvent{
          .kind = aquila::core::OrderResponseKind::kCancelAccepted,
          .local_order_id = stats.open_local_order_id,
      },
      context);

  EXPECT_FALSE(strategy.ShouldStop());
  EXPECT_FALSE(stats.completed);
  EXPECT_EQ(stats.state, LiveUnfilledCancelSmokeState::kCancelPending);
}

TEST(LeadLagLiveStrategyTest,
     SmokeUnfilledCancelSubmitsCancelOnlyOnceWhenBothAcceptedSignalsArrive) {
  LiveUnfilledCancelSmokeStats stats;
  LiveUnfilledCancelSmokeStrategy strategy{
      MakeLeadLagConfig(),
      LiveUnfilledCancelSmokeOptions{
          .symbol = "BTC_USDT",
          .passive_price_bps = 500.0,
          .max_notional = 2000.0,
      },
      &stats,
  };
  SmokeStrategyContext context;

  strategy.OnBookTicker(
      aquila::BookTicker{
          .symbol_id = 3,
          .exchange = aquila::Exchange::kGate,
          .bid_price = 100.0,
          .ask_price = 101.0,
      },
      context);
  strategy.OnOrderResponse(
      aquila::core::OrderResponseEvent{
          .kind = aquila::core::OrderResponseKind::kAccepted,
          .local_order_id = stats.open_local_order_id,
      },
      context);
  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kAccepted,
          .local_order_id = stats.open_local_order_id,
      },
      context);

  ASSERT_EQ(context.cancelled_local_order_ids.size(), 1U);
  EXPECT_EQ(context.cancelled_local_order_ids[0], stats.open_local_order_id);
}

TEST(LeadLagLiveStrategyTest,
     SmokeUnfilledCancelTreatsAnyFillBeforeCancelAsFailure) {
  LiveUnfilledCancelSmokeStats stats;
  LiveUnfilledCancelSmokeStrategy strategy{
      MakeLeadLagConfig(),
      LiveUnfilledCancelSmokeOptions{
          .symbol = "BTC_USDT",
          .passive_price_bps = 500.0,
          .max_notional = 2000.0,
      },
      &stats,
  };
  SmokeStrategyContext context;

  strategy.OnBookTicker(
      aquila::BookTicker{
          .symbol_id = 3,
          .exchange = aquila::Exchange::kGate,
          .bid_price = 100.0,
          .ask_price = 101.0,
      },
      context);
  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kFilled,
          .local_order_id = stats.open_local_order_id,
          .cumulative_filled_quantity = 10,
      },
      context);

  EXPECT_TRUE(strategy.ShouldStop());
  EXPECT_EQ(stats.state, LiveUnfilledCancelSmokeState::kError);
  EXPECT_NE(stats.error.find("unexpected fill"), std::string::npos);
}

TEST(LeadLagLiveStrategyTest,
     SmokeUnfilledCancelRejectsUnexpectedCancelledQuantity) {
  LiveUnfilledCancelSmokeStats stats;
  LiveUnfilledCancelSmokeStrategy strategy{
      MakeLeadLagConfig(),
      LiveUnfilledCancelSmokeOptions{
          .symbol = "BTC_USDT",
          .passive_price_bps = 500.0,
          .max_notional = 2000.0,
      },
      &stats,
  };
  SmokeStrategyContext context;

  strategy.OnBookTicker(
      aquila::BookTicker{
          .symbol_id = 3,
          .exchange = aquila::Exchange::kGate,
          .bid_price = 100.0,
          .ask_price = 101.0,
      },
      context);
  strategy.OnOrderResponse(
      aquila::core::OrderResponseEvent{
          .kind = aquila::core::OrderResponseKind::kAccepted,
          .local_order_id = stats.open_local_order_id,
      },
      context);
  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kCancelled,
          .local_order_id = stats.open_local_order_id,
          .cumulative_filled_quantity = 0,
          .cancelled_quantity = 9,
      },
      context);

  EXPECT_TRUE(strategy.ShouldStop());
  EXPECT_FALSE(stats.completed);
  EXPECT_EQ(stats.state, LiveUnfilledCancelSmokeState::kError);
  EXPECT_NE(stats.error.find("unexpected cancelled quantity"),
            std::string::npos);
}

}  // namespace
}  // namespace aquila::tools::lead_lag
