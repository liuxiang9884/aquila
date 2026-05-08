#include "core/config/order_feedback_shm_config.h"

#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

aquila::config::OrderFeedbackShmConfigResult ParseOrderFeedbackShmToml(
    std::string_view text) {
  const toml::parse_result parsed = toml::parse(text);
  return aquila::config::ParseOrderFeedbackShmConfig(parsed);
}

TEST(OrderFeedbackShmConfigTest, LoadsReadyOrderFeedbackShmConfig) {
  const auto result = aquila::config::LoadOrderFeedbackShmConfigFile(
      SourcePath("config/order_feedback/gate_order_feedback_shm.toml"));
  ASSERT_TRUE(result.ok) << result.error;

  const aquila::config::OrderFeedbackShmRuntimeConfig& config = result.value;
  EXPECT_EQ(config.shm_name, "aquila_gate_order_feedback");
  EXPECT_EQ(config.channel_name, "orders");
  EXPECT_EQ(config.max_strategy_count,
            aquila::config::kOrderFeedbackShmMaxStrategyCount);
  EXPECT_EQ(config.queue_capacity,
            aquila::config::kOrderFeedbackShmQueueCapacity);
  EXPECT_TRUE(config.create);
  EXPECT_FALSE(config.remove_existing);

  const toml::parse_result parsed =
      toml::parse_file(
          SourcePath("config/order_feedback/gate_order_feedback_shm.toml")
              .string());
  const toml::table* section = parsed["order_feedback_shm"].as_table();
  ASSERT_NE(section, nullptr);
  EXPECT_FALSE(section->contains("heartbeat_interval_ms"));
  EXPECT_FALSE(section->contains("stale_consumer_timeout_ms"));
}

TEST(OrderFeedbackShmConfigTest, ParseDefaultsOptionalFields) {
  const auto result = ParseOrderFeedbackShmToml(R"toml(
[order_feedback_shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
)toml");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.shm_name, "aquila_gate_order_feedback");
  EXPECT_EQ(result.value.channel_name, "orders");
  EXPECT_EQ(result.value.max_strategy_count,
            aquila::config::kOrderFeedbackShmMaxStrategyCount);
  EXPECT_EQ(result.value.queue_capacity,
            aquila::config::kOrderFeedbackShmQueueCapacity);
  EXPECT_TRUE(result.value.create);
  EXPECT_FALSE(result.value.remove_existing);
}

TEST(OrderFeedbackShmConfigTest, RejectsMissingSection) {
  const auto result = ParseOrderFeedbackShmToml(R"toml(
[other]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("order_feedback_shm"), std::string::npos);
}

TEST(OrderFeedbackShmConfigTest, RejectsMissingShmName) {
  const auto result = ParseOrderFeedbackShmToml(R"toml(
[order_feedback_shm]
channel_name = "orders"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("shm_name"), std::string::npos);
}

TEST(OrderFeedbackShmConfigTest, RejectsMissingChannelName) {
  const auto result = ParseOrderFeedbackShmToml(R"toml(
[order_feedback_shm]
shm_name = "aquila_gate_order_feedback"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("channel_name"), std::string::npos);
}

TEST(OrderFeedbackShmConfigTest, RejectsUnsupportedMaxStrategyCount) {
  const auto result = ParseOrderFeedbackShmToml(R"toml(
[order_feedback_shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
max_strategy_count = 7
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("max_strategy_count"), std::string::npos);
}

TEST(OrderFeedbackShmConfigTest, RejectsUnsupportedQueueCapacity) {
  const auto result = ParseOrderFeedbackShmToml(R"toml(
[order_feedback_shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
queue_capacity = 1024
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("queue_capacity"), std::string::npos);
}

TEST(OrderFeedbackShmConfigTest, RejectsRemoveExistingWithoutCreate) {
  const auto result = ParseOrderFeedbackShmToml(R"toml(
[order_feedback_shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
create = false
remove_existing = true
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("remove_existing"), std::string::npos);
}

}  // namespace
