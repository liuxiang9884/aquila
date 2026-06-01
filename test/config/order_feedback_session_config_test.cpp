#include "exchange/gate/trading/order_feedback_session_config.h"

#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/config/order_feedback_shm_config.h"
#include "nova/utils/log.h"

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

aquila::gate::OrderFeedbackSessionConfigResult ParseConfigToml(
    std::string_view text) {
  const toml::parse_result parsed = toml::parse(text);
  return aquila::gate::ParseOrderFeedbackSessionConfig(parsed);
}

void ExpectSingleGateSizeDecimalHeader(
    const aquila::websocket::ConnectionConfig& connection) {
  ASSERT_EQ(connection.extra_headers.size(), 1u);
  EXPECT_EQ(connection.extra_headers[0].name, "X-Gate-Size-Decimal");
  EXPECT_EQ(connection.extra_headers[0].value, "1");
}

TEST(OrderFeedbackSessionConfigTest,
     LoadsCheckedInGateOrderFeedbackSessionConfig) {
  const auto result = aquila::gate::LoadOrderFeedbackSessionConfigFile(
      SourcePath("config/order_feedback/gate_order_feedback_session.toml"));
  ASSERT_TRUE(result.ok) << result.error;

  const aquila::gate::OrderFeedbackSessionConfig& config = result.value;
  EXPECT_EQ(config.name, "gate_order_feedback_session");
  EXPECT_EQ(config.credentials.api_key_env, "PROBE_KEY");
  EXPECT_EQ(config.credentials.api_secret_env, "PROBE_SECRET");

  EXPECT_EQ(config.connection.host, "fx-ws.gateio.ws");
  EXPECT_EQ(config.connection.port, "443");
  EXPECT_TRUE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.target, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 4);
  ExpectSingleGateSizeDecimalHeader(config.connection);

  EXPECT_EQ(config.shm.shm_name, "aquila_gate_order_feedback");
  EXPECT_EQ(config.shm.channel_name, "orders");
  EXPECT_EQ(config.shm.max_strategy_count,
            aquila::config::kOrderFeedbackShmMaxStrategyCount);
  EXPECT_EQ(config.shm.queue_capacity,
            aquila::config::kOrderFeedbackShmQueueCapacity);
  EXPECT_TRUE(config.shm.create);
  EXPECT_FALSE(config.shm.remove_existing);
}

TEST(OrderFeedbackSessionConfigTest,
     LoadsCheckedInLabUsdtPrivatePlainOrderFeedbackSessionConfig) {
  const auto result = aquila::gate::LoadOrderFeedbackSessionConfigFile(
      SourcePath("config/order_feedback/"
                 "gate_order_feedback_session_lab_usdt_private_plain_20260601"
                 ".toml"));
  ASSERT_TRUE(result.ok) << result.error;

  const aquila::gate::OrderFeedbackSessionConfig& config = result.value;
  EXPECT_EQ(config.name,
            "gate_order_feedback_session_lab_usdt_private_plain_20260601");
  EXPECT_EQ(config.credentials.api_key_env, "PROBE_KEY");
  EXPECT_EQ(config.credentials.api_secret_env, "PROBE_SECRET");

  EXPECT_EQ(config.connection.host, "fxws-private.gateapi.io");
  EXPECT_EQ(config.connection.connect_ip, "10.0.1.154");
  EXPECT_EQ(config.connection.port, "80");
  EXPECT_FALSE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.target, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 6);
  ExpectSingleGateSizeDecimalHeader(config.connection);

  EXPECT_EQ(config.shm.shm_name,
            "aquila_gate_order_feedback_lab_usdt_20260601");
  EXPECT_EQ(config.shm.channel_name, "orders");
  EXPECT_EQ(config.shm.max_strategy_count,
            aquila::config::kOrderFeedbackShmMaxStrategyCount);
  EXPECT_EQ(config.shm.queue_capacity,
            aquila::config::kOrderFeedbackShmQueueCapacity);
  EXPECT_TRUE(config.shm.create);
  EXPECT_TRUE(config.shm.remove_existing);
}

TEST(OrderFeedbackSessionConfigTest,
     LoadsGateOrderFeedbackSessionLogConfigFromToml) {
  const toml::table toml = toml::parse_file(
      SourcePath("config/order_feedback/gate_order_feedback_session.toml")
          .string());

  nova::LogConfig log_config;
  log_config.FromToml(toml["log"]);

  EXPECT_EQ(log_config.log_level(), nova::LogLevel::kLogInfo);
  EXPECT_EQ(log_config.file_sink_name(),
            "/home/liuxiang/log/gate_order_feedback_session.log");
  EXPECT_EQ(log_config.console_sink_name(),
            "gate_order_feedback_session_console");
  EXPECT_EQ(log_config.backend_thread_name(),
            "gate_order_feedback_session_log");
  EXPECT_EQ(log_config.backend_cpu_affinity(), 5);
  EXPECT_EQ(log_config.format_pattern(),
            "%(log_level_short_code)%(time) %(process_id):%(thread_id) "
            "%(file_name):%(caller_function):%(line_number)] %(message)");
  EXPECT_EQ(log_config.timestamp_pattern(), "%Y-%m-%d %H:%M:%S.%Qns");
}

TEST(OrderFeedbackSessionConfigTest, ParsesSettleTargetAndNestedShmConfig) {
  const auto result = ParseConfigToml(R"toml(
[order_feedback_session]
name = "gate_order_feedback_session"
settle = "btc"

[order_feedback_session.credentials]
api_key_env = "GATE_KEY"
api_secret_env = "GATE_SECRET"

[order_feedback_session.websocket.endpoint]
host = "private-gate.example"
enable_tls = false

[order_feedback_session.websocket.execution_policy]
bind_cpu_id = 7

[order_feedback_session.shm]
shm_name = "aquila_gate_order_feedback_btc"
channel_name = "orders_btc"
max_strategy_count = 8
queue_capacity = 65536
create = false
remove_existing = false
)toml");

  ASSERT_TRUE(result.ok) << result.error;

  EXPECT_EQ(result.value.name, "gate_order_feedback_session");
  EXPECT_EQ(result.value.credentials.api_key_env, "GATE_KEY");
  EXPECT_EQ(result.value.credentials.api_secret_env, "GATE_SECRET");
  EXPECT_EQ(result.value.connection.host, "private-gate.example");
  EXPECT_FALSE(result.value.connection.enable_tls);
  EXPECT_EQ(result.value.connection.target, "/v4/ws/btc/sbe?sbe_schema_id=1");
  EXPECT_EQ(result.value.connection.runtime_policy.io_cpu_id, 7);
  ExpectSingleGateSizeDecimalHeader(result.value.connection);
  EXPECT_EQ(result.value.shm.shm_name, "aquila_gate_order_feedback_btc");
  EXPECT_EQ(result.value.shm.channel_name, "orders_btc");
  EXPECT_FALSE(result.value.shm.create);
  EXPECT_FALSE(result.value.shm.remove_existing);
}

TEST(OrderFeedbackSessionConfigTest, RejectsMissingCredentials) {
  const auto result = ParseConfigToml(R"toml(
[order_feedback_session]
name = "gate_order_feedback_session"
settle = "usdt"

[order_feedback_session.websocket.endpoint]
host = "fx-ws.gateio.ws"

[order_feedback_session.websocket.execution_policy]
bind_cpu_id = 4

[order_feedback_session.shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("credentials.api_key_env"), std::string::npos);
}

TEST(OrderFeedbackSessionConfigTest, RejectsUnsupportedShmLaneCount) {
  const auto result = ParseConfigToml(R"toml(
[order_feedback_session]
name = "gate_order_feedback_session"
settle = "usdt"

[order_feedback_session.credentials]
api_key_env = "GATE_KEY"
api_secret_env = "GATE_SECRET"

[order_feedback_session.websocket.endpoint]
host = "fx-ws.gateio.ws"

[order_feedback_session.websocket.execution_policy]
bind_cpu_id = 4

[order_feedback_session.shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
max_strategy_count = 7
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("max_strategy_count"), std::string::npos);
}

TEST(OrderFeedbackSessionConfigTest, RejectsMismatchedExplicitWebSocketTarget) {
  const auto result = ParseConfigToml(R"toml(
[order_feedback_session]
name = "gate_order_feedback_session"
settle = "usdt"

[order_feedback_session.credentials]
api_key_env = "GATE_KEY"
api_secret_env = "GATE_SECRET"

[order_feedback_session.websocket]
target = "/v4/ws/usdt"

[order_feedback_session.websocket.endpoint]
host = "fx-ws.gateio.ws"

[order_feedback_session.websocket.execution_policy]
bind_cpu_id = 4

[order_feedback_session.shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("websocket.target"), std::string::npos);
}

TEST(OrderFeedbackSessionConfigTest, RejectsRemoveExistingWithoutCreate) {
  const auto result = ParseConfigToml(R"toml(
[order_feedback_session]
name = "gate_order_feedback_session"
settle = "usdt"

[order_feedback_session.credentials]
api_key_env = "GATE_KEY"
api_secret_env = "GATE_SECRET"

[order_feedback_session.websocket.endpoint]
host = "fx-ws.gateio.ws"

[order_feedback_session.websocket.execution_policy]
bind_cpu_id = 4

[order_feedback_session.shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
create = false
remove_existing = true
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("remove_existing"), std::string::npos);
}

}  // namespace
