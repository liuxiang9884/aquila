#include "tools/lead_lag/live_strategy.h"

#include <string>

#include <gtest/gtest.h>

#include "core/config/strategy_config.h"
#include "strategy/lead_lag/execution_state.h"

namespace aquila::tools::lead_lag {
namespace {

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

}  // namespace
}  // namespace aquila::tools::lead_lag
