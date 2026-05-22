#include "tools/lead_lag/live_strategy.h"

#include <string>

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

}  // namespace
}  // namespace aquila::tools::lead_lag
