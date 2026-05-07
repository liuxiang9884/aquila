#include "exchange/gate/trading/order_session_config.h"

#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "nova/utils/log.h"

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

TEST(OrderSessionConfigTest, LoadsCheckedInGateOrderSessionConfig) {
  const auto result = aquila::gate::LoadOrderSessionConfigFile(
      SourcePath("config/order_sessions/gate_order_session.toml"));
  ASSERT_TRUE(result.ok) << result.error;

  const aquila::gate::OrderSessionConfig& config = result.value;
  EXPECT_EQ(config.name, "gate_order_session");
  EXPECT_EQ(config.credentials.api_key_env, "TEST_KEY");
  EXPECT_EQ(config.credentials.api_secret_env, "TEST_SECRET");
  EXPECT_EQ(config.request_map_capacity,
            aquila::gate::kDefaultOrderRequestMapCapacity);

  EXPECT_EQ(config.connection.host, "fx-ws.gateio.ws");
  EXPECT_EQ(config.connection.service, "443");
  EXPECT_TRUE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.target, "/v4/ws/usdt");
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 4);
}

TEST(OrderSessionConfigTest, LoadsGateOrderSessionLogConfigFromToml) {
  const toml::table toml = toml::parse_file(
      SourcePath("config/order_sessions/gate_order_session.toml").string());

  nova::LogConfig log_config;
  log_config.FromToml(toml["log"]);

  EXPECT_EQ(log_config.log_level(), nova::LogLevel::kLogInfo);
  EXPECT_EQ(log_config.file_sink_name(),
            "/home/liuxiang/log/gate_order_session.log");
  EXPECT_EQ(log_config.console_sink_name(), "gate_order_session_console");
  EXPECT_EQ(log_config.backend_thread_name(), "gate_order_session_log");
  EXPECT_EQ(log_config.backend_cpu_affinity(), 5);
  EXPECT_EQ(log_config.format_pattern(),
            "%(log_level_short_code)%(time) %(process_id):%(thread_id) "
            "%(file_name):%(caller_function):%(line_number)] %(message)");
  EXPECT_EQ(log_config.timestamp_pattern(), "%Y-%m-%d %H:%M:%S.%Qns");
}

TEST(OrderSessionConfigTest, ParsesOptionalRequestMapCapacityAndSettleTarget) {
  const toml::parse_result toml = toml::parse(R"toml(
[order_session]
name = "gate_order_session"
settle = "btc"
request_map_capacity = 1024

[order_session.credentials]
api_key_env = "GATE_KEY"
api_secret_env = "GATE_SECRET"

[order_session.websocket.endpoint]
host = "private-gate.example"
enable_tls = false

[order_session.websocket.execution_policy]
bind_cpu_id = 7
)toml");

  const auto result = aquila::gate::ParseOrderSessionConfig(toml);
  ASSERT_TRUE(result.ok) << result.error;

  EXPECT_EQ(result.value.name, "gate_order_session");
  EXPECT_EQ(result.value.credentials.api_key_env, "GATE_KEY");
  EXPECT_EQ(result.value.credentials.api_secret_env, "GATE_SECRET");
  EXPECT_EQ(result.value.request_map_capacity, 1024u);
  EXPECT_EQ(result.value.connection.host, "private-gate.example");
  EXPECT_FALSE(result.value.connection.enable_tls);
  EXPECT_EQ(result.value.connection.target, "/v4/ws/btc");
  EXPECT_EQ(result.value.connection.runtime_policy.io_cpu_id, 7);
}

TEST(OrderSessionConfigTest, RejectsMissingCredentials) {
  const toml::parse_result toml = toml::parse(R"toml(
[order_session]
name = "gate_order_session"
settle = "usdt"

[order_session.websocket.endpoint]
host = "fx-ws.gateio.ws"

[order_session.websocket.execution_policy]
bind_cpu_id = 4
)toml");

  const auto result = aquila::gate::ParseOrderSessionConfig(toml);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("credentials.api_key_env"), std::string::npos);
}

TEST(OrderSessionConfigTest, RejectsZeroRequestMapCapacity) {
  const toml::parse_result toml = toml::parse(R"toml(
[order_session]
name = "gate_order_session"
settle = "usdt"
request_map_capacity = 0

[order_session.credentials]
api_key_env = "GATE_KEY"
api_secret_env = "GATE_SECRET"

[order_session.websocket.endpoint]
host = "fx-ws.gateio.ws"

[order_session.websocket.execution_policy]
bind_cpu_id = 4
)toml");

  const auto result = aquila::gate::ParseOrderSessionConfig(toml);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("request_map_capacity"), std::string::npos);
}

}  // namespace
