#include "core/config/order_gateway_config.h"

#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

aquila::config::OrderGatewayConfigResult ParseOrderGatewayToml(
    std::string_view text) {
  const toml::parse_result parsed = toml::parse(text);
  return aquila::config::ParseOrderGatewayConfig(parsed);
}

std::string MinimalOrderGatewayToml() {
  return R"toml(
[order_gateway]
name = "gate_order_gateway_test"
route_count = 4
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30

[[order_gateway.routes]]
name = "route0"
order_session_config = "config/order_sessions/gate_order_session.toml"
worker_cpu_id = 4

[[order_gateway.routes]]
name = "route1"
order_session_config = "config/order_sessions/gate_order_session.toml"
worker_cpu_id = 5

[[order_gateway.routes]]
name = "route2"
order_session_config = "config/order_sessions/gate_order_session.toml"
worker_cpu_id = 6

[[order_gateway.routes]]
name = "route3"
order_session_config = "config/order_sessions/gate_order_session.toml"
worker_cpu_id = 7
)toml";
}

TEST(OrderGatewayConfigTest, ParsesMinimalFourRouteGateway) {
  const auto result = ParseOrderGatewayToml(MinimalOrderGatewayToml());

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::OrderGatewayConfig& config = result.value;
  EXPECT_EQ(config.name, "gate_order_gateway_test");
  EXPECT_EQ(config.shm_name, "gate_order_gateway_test");
  EXPECT_EQ(config.route_count, 4U);
  EXPECT_EQ(config.command_queue_capacity, 4096U);
  EXPECT_EQ(config.event_queue_capacity, 8192U);
  EXPECT_EQ(config.startup_ready_timeout_s, 30U);
  ASSERT_EQ(config.routes.size(), 4U);
  EXPECT_EQ(config.routes[0].name, "route0");
  EXPECT_EQ(config.routes[0].order_session_config_path,
            std::filesystem::path("config/order_sessions/"
                                  "gate_order_session.toml"));
  EXPECT_EQ(config.routes[0].worker_cpu_id, 4);
  EXPECT_EQ(config.routes[3].name, "route3");
  EXPECT_EQ(config.routes[3].worker_cpu_id, 7);
}

TEST(OrderGatewayConfigTest, ResolvesRoutePathsRelativeToConfigFile) {
  const toml::parse_result parsed = toml::parse(MinimalOrderGatewayToml());
  const auto result = aquila::config::ParseOrderGatewayConfig(
      parsed, SourcePath("config/order_gateways/gateway.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.routes[0].order_session_config_path,
            SourcePath("config/order_sessions/gate_order_session.toml"));
}

TEST(OrderGatewayConfigTest, RejectsRouteCountOutsideRange) {
  std::string zero_route = MinimalOrderGatewayToml();
  const std::string needle = "route_count = 4";
  zero_route.replace(zero_route.find(needle), needle.size(), "route_count = 0");
  auto zero_result = ParseOrderGatewayToml(zero_route);
  EXPECT_FALSE(zero_result.ok);
  EXPECT_NE(zero_result.error.find("route_count"), std::string::npos);

  std::string too_many_routes = MinimalOrderGatewayToml();
  too_many_routes.replace(too_many_routes.find(needle), needle.size(),
                          "route_count = 17");
  auto too_many_result = ParseOrderGatewayToml(too_many_routes);
  EXPECT_FALSE(too_many_result.ok);
  EXPECT_NE(too_many_result.error.find("route_count"), std::string::npos);
}

TEST(OrderGatewayConfigTest, RejectsRouteCountMismatch) {
  std::string text = MinimalOrderGatewayToml();
  const std::string needle = "route_count = 4";
  text.replace(text.find(needle), needle.size(), "route_count = 3");

  const auto result = ParseOrderGatewayToml(text);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("routes.size"), std::string::npos);
}

TEST(OrderGatewayConfigTest, RejectsNonPositiveCapacitiesAndTimeout) {
  std::string command_capacity = MinimalOrderGatewayToml();
  const std::string command_needle = "command_queue_capacity = 4096";
  command_capacity.replace(command_capacity.find(command_needle),
                           command_needle.size(), "command_queue_capacity = 0");
  auto command_result = ParseOrderGatewayToml(command_capacity);
  EXPECT_FALSE(command_result.ok);
  EXPECT_NE(command_result.error.find("command_queue_capacity"),
            std::string::npos);

  std::string event_capacity = MinimalOrderGatewayToml();
  const std::string event_needle = "event_queue_capacity = 8192";
  event_capacity.replace(event_capacity.find(event_needle), event_needle.size(),
                         "event_queue_capacity = 0");
  auto event_result = ParseOrderGatewayToml(event_capacity);
  EXPECT_FALSE(event_result.ok);
  EXPECT_NE(event_result.error.find("event_queue_capacity"), std::string::npos);

  std::string timeout = MinimalOrderGatewayToml();
  const std::string timeout_needle = "startup_ready_timeout_s = 30";
  timeout.replace(timeout.find(timeout_needle), timeout_needle.size(),
                  "startup_ready_timeout_s = 0");
  auto timeout_result = ParseOrderGatewayToml(timeout);
  EXPECT_FALSE(timeout_result.ok);
  EXPECT_NE(timeout_result.error.find("startup_ready_timeout_s"),
            std::string::npos);
}

TEST(OrderGatewayConfigTest, RejectsMissingWorkerCpuId) {
  std::string text = MinimalOrderGatewayToml();
  const std::string needle = "\nworker_cpu_id = 4";
  text.erase(text.find(needle), needle.size());

  const auto result = ParseOrderGatewayToml(text);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("worker_cpu_id"), std::string::npos);
}

TEST(OrderGatewayConfigTest, AllowsExplicitShmNameOverride) {
  std::string text = MinimalOrderGatewayToml();
  const std::string needle = "name = \"gate_order_gateway_test\"";
  text.replace(text.find(needle), needle.size(),
               "name = \"gate_order_gateway_test\"\n"
               "shm_name = \"aquila_gate_order_gateway_shm\"");

  const auto result = ParseOrderGatewayToml(text);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "gate_order_gateway_test");
  EXPECT_EQ(result.value.shm_name, "aquila_gate_order_gateway_shm");
}

}  // namespace
