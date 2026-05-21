#include "tools/lead_lag/live_strategy.h"

#include <string>

#include <gtest/gtest.h>

#include "core/config/strategy_config.h"

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

}  // namespace
}  // namespace aquila::tools::lead_lag
