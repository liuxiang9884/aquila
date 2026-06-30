#ifndef AQUILA_CORE_CONFIG_ORDER_GATEWAY_CONFIG_H_
#define AQUILA_CORE_CONFIG_ORDER_GATEWAY_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/trading/order_gateway_shm_types.h"

namespace aquila::config {

struct OrderGatewayRouteConfig {
  std::string name;
  std::filesystem::path order_session_config_path;
  std::int32_t worker_cpu_id{-1};
};

struct OrderGatewayConfig {
  std::string name;
  std::string shm_name;
  std::uint16_t route_count{0};
  std::uint32_t command_queue_capacity{4096};
  std::uint32_t event_queue_capacity{8192};
  std::uint32_t startup_ready_timeout_s{30};
  std::vector<OrderGatewayRouteConfig> routes;
};

using OrderGatewayConfigResult = Result<OrderGatewayConfig>;

[[nodiscard]] OrderGatewayConfigResult ParseOrderGatewayConfig(
    const toml::table& node);

[[nodiscard]] OrderGatewayConfigResult ParseOrderGatewayConfig(
    const toml::table& node, const std::filesystem::path& config_file_path);

[[nodiscard]] OrderGatewayConfigResult LoadOrderGatewayConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_ORDER_GATEWAY_CONFIG_H_
