#include "core/config/strategy_config.h"

#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

aquila::config::StrategyConfigResult ParseConfigToml(std::string_view text) {
  const toml::parse_result parsed = toml::parse(text);
  return aquila::config::ParseStrategyConfig(parsed);
}

std::string MinimalStrategyConfigToml() {
  return R"toml(
[strategy]
name = "lead_lag"
strategy_id = 4
order_capacity = 8
config = "config/strategies/lead_lag.toml"

[strategy.data_reader]
config = "config/data_readers/strategy_data_reader.toml"

[strategy.order_session]
config = "config/order_sessions/gate_order_session.toml"

[strategy.feedback]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
)toml";
}

TEST(StrategyConfigTest, LoadsCheckedInLeadLagStrategyConfig) {
  const auto result = aquila::config::LoadStrategyConfigFile(
      SourcePath("config/strategies/lead_lag_btc_strategy.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::StrategyConfig& config = result.value;

  EXPECT_EQ(config.name, "lead_lag");
  EXPECT_EQ(config.strategy_id, 4);
  EXPECT_EQ(config.mode, aquila::config::StrategyMode::kDryRun);
  EXPECT_EQ(config.order_capacity, 8U);
  EXPECT_EQ(config.user_config_path,
            SourcePath("config/strategies/lead_lag.toml"));

  EXPECT_EQ(config.loop.idle_policy,
            aquila::config::StrategyLoopIdlePolicy::kSpin);
  EXPECT_EQ(config.loop.bind_cpu_id, 4);
  EXPECT_EQ(config.loop.max_loop_seconds, 0U);

  EXPECT_EQ(config.data_reader.config_path,
            SourcePath("config/data_readers/strategy_data_reader.toml"));
  EXPECT_EQ(config.order_session.config_path,
            SourcePath("config/order_sessions/gate_order_session.toml"));

  EXPECT_TRUE(config.feedback.enabled);
  EXPECT_EQ(config.feedback.shm_name, "aquila_gate_order_feedback");
  EXPECT_EQ(config.feedback.channel_name, "orders");
  EXPECT_EQ(config.feedback.poll_budget, 32U);
  EXPECT_FALSE(config.feedback.force_claim);
}

TEST(StrategyConfigTest, LoadsCheckedInLeadLagFirst5StrategyConfig) {
  const auto result = aquila::config::LoadStrategyConfigFile(
      SourcePath("config/strategies/lead_lag_first5_strategy_20260521.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::StrategyConfig& config = result.value;

  EXPECT_EQ(config.name, "lead_lag");
  EXPECT_EQ(config.strategy_id, 4);
  EXPECT_EQ(config.mode, aquila::config::StrategyMode::kDryRun);
  EXPECT_EQ(config.order_capacity, 16U);
  EXPECT_EQ(config.user_config_path,
            SourcePath("config/strategies/lead_lag_first5_20260521.toml"));
  EXPECT_EQ(
      config.data_reader.config_path,
      SourcePath(
          "config/data_readers/strategy_data_reader_first5_20260521.toml"));
  EXPECT_EQ(config.order_session.config_path,
            SourcePath("config/order_sessions/gate_order_session.toml"));
  EXPECT_FALSE(config.feedback.enabled);
}

TEST(StrategyConfigTest, LoadsCheckedInLeadLagRequestedStrategyConfig) {
  const auto result = aquila::config::LoadStrategyConfigFile(SourcePath(
      "config/strategies/lead_lag_requested_strategy_20260521.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::StrategyConfig& config = result.value;

  EXPECT_EQ(config.name, "lead_lag");
  EXPECT_EQ(config.strategy_id, 4);
  EXPECT_EQ(config.mode, aquila::config::StrategyMode::kDryRun);
  EXPECT_EQ(config.order_capacity, 32U);
  EXPECT_EQ(config.user_config_path,
            SourcePath("config/strategies/lead_lag_requested_20260521.toml"));
  EXPECT_EQ(
      config.data_reader.config_path,
      SourcePath(
          "config/data_readers/strategy_data_reader_requested_20260521.toml"));
  EXPECT_EQ(config.order_session.config_path,
            SourcePath("config/order_sessions/gate_order_session.toml"));
  EXPECT_FALSE(config.feedback.enabled);
}

TEST(StrategyConfigTest, LoadsCheckedInLeadLagRequested12SymbolsRuntimeConfig) {
  const auto result = aquila::config::LoadStrategyConfigFile(
      SourcePath("config/strategies/"
                 "lead_lag_requested_11symbols_strategy_20260522.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::StrategyConfig& config = result.value;

  EXPECT_EQ(config.name, "lead_lag");
  EXPECT_EQ(config.strategy_id, 4);
  EXPECT_EQ(config.mode, aquila::config::StrategyMode::kDryRun);
  EXPECT_EQ(config.order_capacity, 32U);
  EXPECT_EQ(
      config.user_config_path,
      SourcePath(
          "config/strategies/lead_lag_requested_11symbols_20260522.toml"));
  EXPECT_EQ(
      config.data_reader.config_path,
      SourcePath(
          "config/data_readers/strategy_data_reader_requested_20260521.toml"));
  EXPECT_EQ(config.order_session.config_path,
            SourcePath("config/order_sessions/gate_order_session.toml"));
  EXPECT_FALSE(config.feedback.enabled);
}

TEST(StrategyConfigTest,
     LoadsCheckedInLeadLagRequested12SymbolsLiveRuntimeConfig) {
  const auto result = aquila::config::LoadStrategyConfigFile(
      SourcePath("config/strategies/"
                 "lead_lag_requested_11symbols_live_strategy_20260522.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::StrategyConfig& config = result.value;

  EXPECT_EQ(config.name, "lead_lag");
  EXPECT_EQ(config.strategy_id, 4);
  EXPECT_EQ(config.mode, aquila::config::StrategyMode::kLive);
  EXPECT_EQ(config.order_capacity, 32U);
  EXPECT_EQ(
      config.user_config_path,
      SourcePath(
          "config/strategies/lead_lag_requested_11symbols_20260522.toml"));
  EXPECT_EQ(
      config.data_reader.config_path,
      SourcePath(
          "config/data_readers/strategy_data_reader_requested_20260521.toml"));
  EXPECT_EQ(config.order_session.config_path,
            SourcePath("config/order_sessions/gate_order_session.toml"));
  EXPECT_TRUE(config.feedback.enabled);
  EXPECT_EQ(config.feedback.poll_budget, 32U);
}

TEST(StrategyConfigTest, LoadsCheckedInLeadLagLabUsdtLiveRuntimeConfig) {
  const auto result = aquila::config::LoadStrategyConfigFile(SourcePath(
      "config/strategies/lead_lag_lab_usdt_live_strategy_20260601.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::StrategyConfig& config = result.value;

  EXPECT_EQ(config.name, "lead_lag");
  EXPECT_EQ(config.strategy_id, 4);
  EXPECT_EQ(config.mode, aquila::config::StrategyMode::kLive);
  EXPECT_EQ(config.order_capacity, 8U);
  EXPECT_EQ(config.user_config_path,
            SourcePath("config/strategies/lead_lag_lab_usdt_20260601.toml"));
  EXPECT_EQ(
      config.data_reader.config_path,
      SourcePath(
          "config/data_readers/strategy_data_reader_lab_usdt_20260601.toml"));
  EXPECT_EQ(config.order_session.config_path,
            SourcePath("config/order_sessions/"
                       "gate_order_session_lab_usdt_private_plain_20260601"
                       ".toml"));
  EXPECT_TRUE(config.feedback.enabled);
  EXPECT_EQ(config.feedback.shm_name,
            "aquila_gate_order_feedback_lab_usdt_20260601");
  EXPECT_EQ(config.feedback.channel_name, "orders");
  EXPECT_EQ(config.feedback.poll_budget, 32U);
  EXPECT_FALSE(config.feedback.force_claim);
}

TEST(StrategyConfigTest, RejectsStrategyIdOutsideFeedbackLaneRange) {
  const auto result = ParseConfigToml(R"toml(
[strategy]
name = "lead_lag"
strategy_id = 8
order_capacity = 8
config = "config/strategies/lead_lag.toml"

[strategy.data_reader]
config = "config/data_readers/strategy_data_reader.toml"

[strategy.order_session]
config = "config/order_sessions/gate_order_session.toml"

[strategy.feedback]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("strategy.strategy_id"), std::string::npos);
}

TEST(StrategyConfigTest, RejectsZeroOrderCapacity) {
  std::string toml_text = MinimalStrategyConfigToml();
  const std::string needle = "order_capacity = 8";
  toml_text.replace(toml_text.find(needle), needle.size(),
                    "order_capacity = 0");

  const auto result = ParseConfigToml(toml_text);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("strategy.order_capacity"), std::string::npos);
}

TEST(StrategyConfigTest, RejectsMissingStrategyConfigPath) {
  const auto result = ParseConfigToml(R"toml(
[strategy]
name = "lead_lag"
strategy_id = 4
order_capacity = 8

[strategy.data_reader]
config = "config/data_readers/strategy_data_reader.toml"

[strategy.order_session]
config = "config/order_sessions/gate_order_session.toml"

[strategy.feedback]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("strategy.config"), std::string::npos);
}

TEST(StrategyConfigTest, RejectsUnknownMode) {
  const auto result = ParseConfigToml(R"toml(
[strategy]
name = "lead_lag"
strategy_id = 4
mode = "paper"
order_capacity = 8
config = "config/strategies/lead_lag.toml"

[strategy.data_reader]
config = "config/data_readers/strategy_data_reader.toml"

[strategy.order_session]
config = "config/order_sessions/gate_order_session.toml"

[strategy.feedback]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("strategy.mode"), std::string::npos);
}

TEST(StrategyConfigTest, RejectsFeedbackEnabledWithoutShmName) {
  const auto result = ParseConfigToml(R"toml(
[strategy]
name = "lead_lag"
strategy_id = 4
order_capacity = 8
config = "config/strategies/lead_lag.toml"

[strategy.data_reader]
config = "config/data_readers/strategy_data_reader.toml"

[strategy.order_session]
config = "config/order_sessions/gate_order_session.toml"

[strategy.feedback]
enabled = true
channel_name = "orders"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("strategy.feedback.shm_name"), std::string::npos);
}

TEST(StrategyConfigTest, RejectsZeroFeedbackPollBudget) {
  const auto result = ParseConfigToml(R"toml(
[strategy]
name = "lead_lag"
strategy_id = 4
order_capacity = 8
config = "config/strategies/lead_lag.toml"

[strategy.data_reader]
config = "config/data_readers/strategy_data_reader.toml"

[strategy.order_session]
config = "config/order_sessions/gate_order_session.toml"

[strategy.feedback]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
poll_budget = 0
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("strategy.feedback.poll_budget"),
            std::string::npos);
}

TEST(StrategyConfigTest, AcceptsFeedbackDisabledWithoutShmFields) {
  const auto result = ParseConfigToml(R"toml(
[strategy]
name = "lead_lag"
strategy_id = 4
order_capacity = 8
config = "config/strategies/lead_lag.toml"

[strategy.data_reader]
config = "config/data_readers/strategy_data_reader.toml"

[strategy.order_session]
config = "config/order_sessions/gate_order_session.toml"

[strategy.feedback]
enabled = false
)toml");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_FALSE(result.value.feedback.enabled);
  EXPECT_TRUE(result.value.feedback.shm_name.empty());
  EXPECT_TRUE(result.value.feedback.channel_name.empty());
  EXPECT_EQ(result.value.feedback.poll_budget, 32U);
}

}  // namespace
