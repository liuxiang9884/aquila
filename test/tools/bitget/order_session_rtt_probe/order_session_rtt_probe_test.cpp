#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "exchange/bitget/trading/order_session_config.h"
#include "tools/bitget/order_session_rtt_probe/config.h"
#include "tools/bitget/order_session_rtt_probe/connection_plan.h"
#include "tools/bitget/order_session_rtt_probe/run_plan.h"
#include "tools/bitget/order_session_rtt_probe/session_config_builder.h"

namespace aquila::tools::bitget_order_session_rtt_probe {
namespace {

std::filesystem::path UniqueTmpPath(std::string_view stem) {
  static std::atomic<std::uint64_t> sequence{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::path{"/home/liuxiang/tmp"} /
         fmt::format("{}_{}_{}.csv", stem, now,
                     sequence.fetch_add(1, std::memory_order_relaxed));
}

std::filesystem::path WriteConnectionsCsv(std::string_view text) {
  const std::filesystem::path path = UniqueTmpPath("bitget_rtt_connections");
  std::ofstream output(path);
  output << text;
  output.close();
  return path;
}

toml::parse_result ParseMinimalConfig(std::string_view extra) {
  return toml::parse(fmt::format(
      R"toml(
[probe]
name = "bitget_order_session_rtt_probe"
run_id = "unit"

[probe.inputs]
order_session_config = "config/order_sessions/bitget_order_session.toml"
data_reader_config = "config/data_readers/bitget_order_session_rtt_probe.toml"
connections_file = "/home/liuxiang/tmp/bitget_connections.csv"

[probe.output]
root_dir = "/home/liuxiang/tmp/bitget_order_session_rtt_probe"

{}
)toml",
      extra));
}

ProbeConnectionConfig MakeConnection(std::string name, std::string connect_ip,
                                     std::int32_t worker_cpu_id) {
  return ProbeConnectionConfig{
      .name = std::move(name),
      .group = "ha",
      .host = "vip-ws-uta.bitget.com",
      .connect_ip = std::move(connect_ip),
      .port = "443",
      .enable_tls = true,
      .worker_cpu_id = worker_cpu_id,
  };
}

TEST(BitgetRttProbeConfigTest, ParsesIocProbeConfig) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.sessions]
wait_login_timeout_ms = 9000
request_timeout_ms = 4000

[probe.sampling]
samples_per_session = 3
cycle_cooldown_us = 700000
order_session_interval_us = 25000
max_events_per_drain = 64
feedback_queue_capacity = 128
coordinator_cpu = 15

[probe.order]
order_mode = "ioc"
passive_price_limit_fraction = 0.5
bbo_freshness_ns = 500000000

[probe.feedback]
shm_config = "config/order_feedback/bitget_order_feedback_session.toml"
strategy_id = 7
force_claim = true
poll_budget = 32
terminal_timeout_ms = 4500
)toml"));

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "bitget_order_session_rtt_probe");
  EXPECT_EQ(result.value.sampling.samples_per_session, 3U);
  EXPECT_EQ(result.value.sampling.feedback_queue_capacity, 128U);
  EXPECT_EQ(result.value.order.order_mode, "ioc");
  EXPECT_EQ(result.value.order.bbo_freshness_ns, 500000000U);
  EXPECT_EQ(result.value.feedback.strategy_id, 7U);
  EXPECT_TRUE(result.value.feedback.force_claim);
}

TEST(BitgetRttProbeConfigTest, RejectsZeroSamples) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.sampling]
samples_per_session = 0
)toml"));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("samples_per_session"), std::string::npos);
}

TEST(BitgetRttProbeConfigTest, RejectsNonIocMode) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.order]
order_mode = "gtc"
)toml"));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("order_mode"), std::string::npos);
}

TEST(BitgetRttProbeConfigTest, AllowsZeroPacing) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.sampling]
cycle_cooldown_us = 0
order_session_interval_us = 0
)toml"));

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.sampling.cycle_cooldown_us, 0U);
  EXPECT_EQ(result.value.sampling.order_session_interval_us, 0U);
}

TEST(BitgetRttProbeConfigTest, AllowsFeedbackLaneZero) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.feedback]
strategy_id = 0
)toml"));

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.feedback.strategy_id, 0U);
}

TEST(BitgetRttProbeConnectionsTest, AllowsDnsAndPreservesDuplicateIpRows) {
  const std::filesystem::path path = WriteConnectionsCsv(
      "name,group,host,connect_ip,port,enable_tls,worker_cpu_id\n"
      "ha-0,ha,vip-ws-uta.bitget.com,,443,true,6\n"
      "ha-1,ha,vip-ws-uta.bitget.com,10.0.0.1,443,true,7\n"
      "ha-2,ha,vip-ws-uta.bitget.com,10.0.0.1,443,true,8\n");

  const ProbeConnectionsCsvResult result = LoadProbeConnectionsCsvFile(path);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.connections.size(), 3U);
  EXPECT_TRUE(result.connections[0].connect_ip.empty());
  EXPECT_EQ(result.connections[1].connect_ip, "10.0.0.1");
  EXPECT_EQ(result.connections[2].worker_cpu_id, 8);
  std::filesystem::remove(path);
}

TEST(BitgetRttProbeConnectionsTest, RejectsDuplicateNames) {
  const std::filesystem::path path = WriteConnectionsCsv(
      "name,group,host,connect_ip,port,enable_tls,worker_cpu_id\n"
      "ha-0,ha,vip-ws-uta.bitget.com,,443,true,6\n"
      "ha-0,ha,vip-ws-uta.bitget.com,,443,true,7\n");

  const ProbeConnectionsCsvResult result = LoadProbeConnectionsCsvFile(path);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("duplicates name"), std::string::npos);
  std::filesystem::remove(path);
}

TEST(BitgetRttProbeRunPlanTest, BuildsCyclesInConnectionOrder) {
  ProbeConfig config;
  config.sampling.samples_per_session = 2;
  std::vector<ProbeConnectionConfig> connections;
  connections.push_back(MakeConnection("ha-0", "", 6));
  connections.push_back(MakeConnection("ha-1", "10.0.0.1", 7));

  const ProbeRunPlanResult result =
      BuildProbeRunPlan(config, std::move(connections));

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.cycles.size(), 2U);
  ASSERT_EQ(result.value.cycles.front().connections.size(), 2U);
  EXPECT_EQ(result.value.cycles.front().connections[0].name, "ha-0");
  EXPECT_EQ(result.value.cycles.front().connections[1].name, "ha-1");
}

TEST(BitgetRttProbeRunPlanTest, PinnedConfigOverridesConnectionAndCpu) {
  bitget::OrderSessionConfig base;
  base.connection.host = "base.example";
  base.connection.port = "80";
  base.connection.enable_tls = false;
  base.connection.runtime_policy.io_cpu_id = 1;

  const bitget::OrderSessionConfig pinned = BuildPinnedOrderSessionConfig(
      std::move(base), MakeConnection("ha-0", "10.0.0.2", 9));

  EXPECT_EQ(pinned.connection.host, "vip-ws-uta.bitget.com");
  EXPECT_EQ(pinned.connection.connect_ip, "10.0.0.2");
  EXPECT_EQ(pinned.connection.port, "443");
  EXPECT_TRUE(pinned.connection.enable_tls);
  EXPECT_EQ(pinned.connection.runtime_policy.io_cpu_id, 9);
}

}  // namespace
}  // namespace aquila::tools::bitget_order_session_rtt_probe
