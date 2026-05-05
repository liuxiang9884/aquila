#include "core/config/websocket_config.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/websocket/runtime_policy.h"

namespace {

aquila::config::WebSocketConfigResult ParseWebSocketToml(
    std::string_view text) {
  const toml::parse_result parsed = toml::parse(text);
  return aquila::config::ParseWebSocketConfig(
      parsed["data_session"]["websocket"]);
}

TEST(WebSocketConfigTest, ParsesRequiredFieldsAndAppliesDefaults) {
  const auto result = ParseWebSocketToml(R"toml(
[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"
enable_tls = false

[data_session.websocket.execution_policy]
bind_cpu_id = 2
)toml");

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::WebSocketConfig& config = result.config;

  EXPECT_EQ(config.endpoint.host, "fx-ws.gateio.ws");
  EXPECT_EQ(config.endpoint.service, "443");
  EXPECT_FALSE(config.endpoint.enable_tls);
  EXPECT_EQ(config.endpoint.connect_timeout_ms, 10'000u);

  EXPECT_EQ(config.execution_policy.bind_cpu_id, 2);
  EXPECT_EQ(config.execution_policy.affinity_mode,
            aquila::websocket::AffinityMode::kBestEffort);
  EXPECT_TRUE(config.execution_policy.lock_memory);
  EXPECT_TRUE(config.execution_policy.prefault_stack);
  EXPECT_TRUE(config.execution_policy.active_spin);
  EXPECT_EQ(config.execution_policy.spin_iterations_before_clock_check, 4096u);

  EXPECT_EQ(config.read_path.max_reads_per_drive, 8u);
  EXPECT_FALSE(config.read_path.read_until_would_block);
  EXPECT_EQ(config.heartbeat.interval_ms, 5000u);
  EXPECT_EQ(config.heartbeat.timeout_ms, 15000u);
  EXPECT_TRUE(config.reconnect.enabled);
  EXPECT_EQ(config.reconnect.initial_backoff_ms, 100u);
  EXPECT_EQ(config.reconnect.max_backoff_ms, 30000u);
  EXPECT_EQ(config.reconnect.backoff_shift_bits, 1u);
  EXPECT_EQ(config.reconnect.jitter_percent, 25u);
  EXPECT_EQ(config.reconnect.max_attempts, 0u);

  const auto connection_result = aquila::config::ToConnectionConfig(
      config, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  ASSERT_TRUE(connection_result.ok) << connection_result.error;
  const aquila::websocket::ConnectionConfig& connection =
      connection_result.config;

  EXPECT_EQ(connection.host, "fx-ws.gateio.ws");
  EXPECT_EQ(connection.service, "443");
  EXPECT_EQ(connection.target, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  EXPECT_FALSE(connection.enable_tls);
  EXPECT_EQ(connection.cold_path_total_timeout_ms, 10'000u);
  EXPECT_EQ(connection.max_reads_per_drive, 8u);
  EXPECT_FALSE(connection.read_until_would_block);
  EXPECT_EQ(connection.heartbeat_interval_ms, 5000u);
  EXPECT_EQ(connection.heartbeat_timeout_ms, 15000u);
  EXPECT_TRUE(connection.reconnect.enabled);
  EXPECT_EQ(connection.reconnect.initial_backoff_ms, 100u);
  EXPECT_EQ(connection.reconnect.max_backoff_ms, 30000u);
  EXPECT_EQ(connection.reconnect.backoff_shift_bits, 1u);
  EXPECT_EQ(connection.reconnect.jitter_percent, 25u);
  EXPECT_EQ(connection.reconnect.max_attempts, 0u);
  EXPECT_EQ(connection.runtime_policy.io_cpu_id, 2);
  EXPECT_EQ(connection.runtime_policy.affinity_mode,
            aquila::websocket::AffinityMode::kBestEffort);
  EXPECT_TRUE(connection.runtime_policy.lock_memory);
  EXPECT_TRUE(connection.runtime_policy.prefault_stack);
  EXPECT_TRUE(connection.runtime_policy.active_spin);
  EXPECT_EQ(connection.runtime_policy.spin_iterations_before_clock_check,
            4096u);
}

TEST(WebSocketConfigTest, ParsesOptionalSectionsAndMapsAllFields) {
  const auto result = ParseWebSocketToml(R"toml(
[data_session.websocket.endpoint]
host = "fstream.binance.com"
service = "9443"
enable_tls = true
connect_timeout_ms = 2500

[data_session.websocket.execution_policy]
bind_cpu_id = 3
affinity_mode = "required"
lock_memory = false
prefault_stack = false
active_spin = false
spin_iterations_before_clock_check = 128

[data_session.websocket.read_path]
max_reads_per_drive = 16
read_until_would_block = true

[data_session.websocket.heartbeat]
interval_ms = 7000
timeout_ms = 21000

[data_session.websocket.reconnect]
enabled = false
initial_backoff_ms = 50
max_backoff_ms = 5000
backoff_shift_bits = 2
jitter_percent = 10
max_attempts = 7
)toml");

  ASSERT_TRUE(result.ok) << result.error;

  const auto connection_result = aquila::config::ToConnectionConfig(
      result.config, "/public/ws/btcusdt@bookTicker");
  ASSERT_TRUE(connection_result.ok) << connection_result.error;
  const aquila::websocket::ConnectionConfig& connection =
      connection_result.config;

  EXPECT_EQ(connection.host, "fstream.binance.com");
  EXPECT_EQ(connection.service, "9443");
  EXPECT_EQ(connection.target, "/public/ws/btcusdt@bookTicker");
  EXPECT_TRUE(connection.enable_tls);
  EXPECT_EQ(connection.cold_path_total_timeout_ms, 2500u);
  EXPECT_EQ(connection.max_reads_per_drive, 16u);
  EXPECT_TRUE(connection.read_until_would_block);
  EXPECT_EQ(connection.heartbeat_interval_ms, 7000u);
  EXPECT_EQ(connection.heartbeat_timeout_ms, 21000u);
  EXPECT_FALSE(connection.reconnect.enabled);
  EXPECT_EQ(connection.reconnect.initial_backoff_ms, 50u);
  EXPECT_EQ(connection.reconnect.max_backoff_ms, 5000u);
  EXPECT_EQ(connection.reconnect.backoff_shift_bits, 2u);
  EXPECT_EQ(connection.reconnect.jitter_percent, 10u);
  EXPECT_EQ(connection.reconnect.max_attempts, 7u);
  EXPECT_EQ(connection.runtime_policy.io_cpu_id, 3);
  EXPECT_EQ(connection.runtime_policy.affinity_mode,
            aquila::websocket::AffinityMode::kRequired);
  EXPECT_FALSE(connection.runtime_policy.lock_memory);
  EXPECT_FALSE(connection.runtime_policy.prefault_stack);
  EXPECT_FALSE(connection.runtime_policy.active_spin);
  EXPECT_EQ(connection.runtime_policy.spin_iterations_before_clock_check, 128u);
}

TEST(WebSocketConfigTest, ParsesCheckedInGateMarketDataConfig) {
  const auto parsed =
      toml::parse_file((std::filesystem::path{AQUILA_SOURCE_DIR} /
                        "config/data_sessions/gate_future_market_data.toml")
                           .string());

  const auto result =
      aquila::config::ParseWebSocketConfig(parsed["data_session"]["websocket"]);
  ASSERT_TRUE(result.ok) << result.error;

  EXPECT_EQ(result.config.endpoint.host, "fx-ws.gateio.ws");
  EXPECT_FALSE(result.config.endpoint.enable_tls);
  EXPECT_EQ(result.config.execution_policy.bind_cpu_id, 2);

  const auto connection_result = aquila::config::ToConnectionConfig(
      result.config, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  ASSERT_TRUE(connection_result.ok) << connection_result.error;
  EXPECT_EQ(connection_result.config.target, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  EXPECT_FALSE(connection_result.config.enable_tls);
}

TEST(WebSocketConfigTest, ParsesCheckedInBinanceMarketDataConfig) {
  const auto parsed =
      toml::parse_file((std::filesystem::path{AQUILA_SOURCE_DIR} /
                        "config/data_sessions/binance_future_market_data.toml")
                           .string());

  const auto result =
      aquila::config::ParseWebSocketConfig(parsed["data_session"]["websocket"]);
  ASSERT_TRUE(result.ok) << result.error;

  EXPECT_EQ(result.config.endpoint.host, "fstream.binance.com");
  EXPECT_TRUE(result.config.endpoint.enable_tls);
  EXPECT_EQ(result.config.execution_policy.bind_cpu_id, 3);

  const auto connection_result = aquila::config::ToConnectionConfig(
      result.config, "/public/ws/btcusdt@bookTicker");
  ASSERT_TRUE(connection_result.ok) << connection_result.error;
  EXPECT_EQ(connection_result.config.target, "/public/ws/btcusdt@bookTicker");
  EXPECT_TRUE(connection_result.config.enable_tls);
}

TEST(WebSocketConfigTest, RejectsMissingEndpointHost) {
  const auto result = ParseWebSocketToml(R"toml(
[data_session.websocket.endpoint]

[data_session.websocket.execution_policy]
bind_cpu_id = 2
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("endpoint.host"), std::string::npos);
}

TEST(WebSocketConfigTest, RejectsMissingBindCpuId) {
  const auto result = ParseWebSocketToml(R"toml(
[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"

[data_session.websocket.execution_policy]
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("bind_cpu_id"), std::string::npos);
}

TEST(WebSocketConfigTest, RejectsInvalidAffinityMode) {
  const auto result = ParseWebSocketToml(R"toml(
[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"

[data_session.websocket.execution_policy]
bind_cpu_id = 2
affinity_mode = "strict"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("affinity_mode"), std::string::npos);
}

TEST(WebSocketConfigTest, RejectsInvalidHeartbeatWindow) {
  const auto result = ParseWebSocketToml(R"toml(
[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"

[data_session.websocket.execution_policy]
bind_cpu_id = 2

[data_session.websocket.heartbeat]
interval_ms = 5000
timeout_ms = 5000
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("heartbeat"), std::string::npos);
}

}  // namespace
