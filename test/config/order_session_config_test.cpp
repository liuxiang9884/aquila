#include "exchange/gate/trading/order_session_config.h"

#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/common/order_ack_diagnostic_level.h"
#include "nova/utils/log.h"

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

void ExpectSingleGateSizeDecimalHeader(
    const aquila::websocket::ConnectionConfig& connection) {
  ASSERT_EQ(connection.extra_headers.size(), 1u);
  EXPECT_EQ(connection.extra_headers[0].name, "X-Gate-Size-Decimal");
  EXPECT_EQ(connection.extra_headers[0].value, "1");
}

TEST(OrderSessionConfigTest, LoadsCheckedInGateOrderSessionConfig) {
  const auto result = aquila::gate::LoadOrderSessionConfigFile(
      SourcePath("config/order_sessions/gate_order_session.toml"));
  ASSERT_TRUE(result.ok) << result.error;

  const aquila::gate::OrderSessionConfig& config = result.value;
  EXPECT_EQ(config.name, "gate_order_session");
  EXPECT_EQ(config.credentials.api_key_env, "PROBE_KEY");
  EXPECT_EQ(config.credentials.api_secret_env, "PROBE_SECRET");
  EXPECT_EQ(config.request_map_capacity,
            aquila::gate::kDefaultOrderRequestMapCapacity);
  EXPECT_FALSE(config.enable_tcp_info_diagnostics);
  EXPECT_EQ(config.ack_latency_diagnostics.ack_rtt_threshold_ns, 20'000'000);
  EXPECT_EQ(
      config.ack_latency_diagnostics.send_to_first_drive_read_threshold_ns,
      3'000'000);
  EXPECT_EQ(config.ack_latency_diagnostics.drive_read_duration_threshold_ns,
            1'000'000);
  EXPECT_EQ(config.ack_latency_diagnostics.diagnostic_window_timeout_ns,
            250'000'000);
  EXPECT_EQ(config.ack_latency_diagnostics.max_logs_per_second, 10U);

  EXPECT_EQ(config.connection.host, "fx-ws.gateio.ws");
  EXPECT_EQ(config.connection.port, "443");
  EXPECT_TRUE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.target, "/v4/ws/usdt");
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 4);
  ExpectSingleGateSizeDecimalHeader(config.connection);
}

TEST(OrderSessionConfigTest,
     LoadsCheckedInLabUsdtPrivatePlainOrderSessionConfig) {
  const auto result = aquila::gate::LoadOrderSessionConfigFile(SourcePath(
      "config/order_sessions/"
      "gate_order_session_lab_usdt_private_plain_20260601.toml"));
  if constexpr (!aquila::core::kOrderAckDiagnosticTcpInfoEnabled) {
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("enable_tcp_info"), std::string::npos);
    return;
  }
  if constexpr (!aquila::core::kOrderAckDiagnosticSocketTimestampingEnabled) {
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("timestamping.enabled"), std::string::npos);
    return;
  }
  ASSERT_TRUE(result.ok) << result.error;

  const aquila::gate::OrderSessionConfig& config = result.value;
  EXPECT_EQ(config.name, "gate_order_session_lab_usdt_private_plain_20260601");
  EXPECT_EQ(config.credentials.api_key_env, "PROBE_KEY");
  EXPECT_EQ(config.credentials.api_secret_env, "PROBE_SECRET");
  EXPECT_TRUE(config.enable_tcp_info_diagnostics);
  EXPECT_EQ(config.ack_latency_diagnostics.ack_rtt_threshold_ns, 5'000'000);
  EXPECT_EQ(
      config.ack_latency_diagnostics.send_to_first_drive_read_threshold_ns,
      3'000'000);
  EXPECT_EQ(config.ack_latency_diagnostics.drive_read_duration_threshold_ns,
            1'000'000);
  EXPECT_EQ(config.ack_latency_diagnostics.diagnostic_window_timeout_ns,
            250'000'000);
  EXPECT_EQ(config.ack_latency_diagnostics.max_logs_per_second, 100U);

  EXPECT_EQ(config.connection.host, "fxws-private.gateapi.io");
  EXPECT_EQ(config.connection.connect_ip, "10.0.1.154");
  EXPECT_EQ(config.connection.port, "80");
  EXPECT_FALSE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.target, "/v4/ws/usdt");
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 4);
  EXPECT_TRUE(config.connection.socket_timestamping.enabled);
  EXPECT_TRUE(config.connection.socket_timestamping.tx_sched);
  EXPECT_TRUE(config.connection.socket_timestamping.tx_software);
  EXPECT_TRUE(config.connection.socket_timestamping.tx_ack);
  EXPECT_TRUE(config.connection.socket_timestamping.rx_software);
  EXPECT_FALSE(config.connection.socket_timestamping.hardware);
  ExpectSingleGateSizeDecimalHeader(config.connection);
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

[order_session.diagnostics]
enable_tcp_info = true
ack_rtt_threshold_ns = 0
send_to_first_drive_read_threshold_ns = 1000
drive_read_duration_threshold_ns = 2000
diagnostic_window_timeout_ns = 3000
max_logs_per_second = 4

[order_session.diagnostics.timestamping]
enabled = true
tx_software = true
tx_ack = true
rx_software = true
max_errqueue_events_per_drain = 16
max_active_probes = 8192

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
  if constexpr (!aquila::core::kOrderAckDiagnosticTcpInfoEnabled) {
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("enable_tcp_info"), std::string::npos);
    return;
  }
  if constexpr (!aquila::core::kOrderAckDiagnosticSocketTimestampingEnabled) {
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("timestamping.enabled"), std::string::npos);
    return;
  }
  ASSERT_TRUE(result.ok) << result.error;

  EXPECT_EQ(result.value.name, "gate_order_session");
  EXPECT_EQ(result.value.credentials.api_key_env, "GATE_KEY");
  EXPECT_EQ(result.value.credentials.api_secret_env, "GATE_SECRET");
  EXPECT_EQ(result.value.request_map_capacity, 1024u);
  EXPECT_TRUE(result.value.enable_tcp_info_diagnostics);
  EXPECT_EQ(result.value.ack_latency_diagnostics.ack_rtt_threshold_ns, 0);
  EXPECT_EQ(result.value.ack_latency_diagnostics
                .send_to_first_drive_read_threshold_ns,
            1000);
  EXPECT_EQ(
      result.value.ack_latency_diagnostics.drive_read_duration_threshold_ns,
      2000);
  EXPECT_EQ(result.value.ack_latency_diagnostics.diagnostic_window_timeout_ns,
            3000);
  EXPECT_EQ(result.value.ack_latency_diagnostics.max_logs_per_second, 4U);
  EXPECT_TRUE(result.value.connection.socket_timestamping.enabled);
  EXPECT_TRUE(result.value.connection.socket_timestamping.tx_software);
  EXPECT_TRUE(result.value.connection.socket_timestamping.tx_ack);
  EXPECT_TRUE(result.value.connection.socket_timestamping.rx_software);
  EXPECT_EQ(
      result.value.connection.socket_timestamping.max_errqueue_events_per_drain,
      16U);
  EXPECT_EQ(result.value.connection.socket_timestamping.max_active_probes,
            8192U);
  EXPECT_EQ(result.value.connection.host, "private-gate.example");
  EXPECT_FALSE(result.value.connection.enable_tls);
  EXPECT_EQ(result.value.connection.target, "/v4/ws/btc");
  EXPECT_EQ(result.value.connection.runtime_policy.io_cpu_id, 7);
  ExpectSingleGateSizeDecimalHeader(result.value.connection);
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

TEST(OrderSessionConfigTest, RejectsNegativeAckLatencyThreshold) {
  const toml::parse_result toml = toml::parse(R"toml(
[order_session]
name = "gate_order_session"
settle = "usdt"

[order_session.diagnostics]
ack_rtt_threshold_ns = -1

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
  EXPECT_NE(result.error.find("ack_rtt_threshold_ns"), std::string::npos);
}

}  // namespace
