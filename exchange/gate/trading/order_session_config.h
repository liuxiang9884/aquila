#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_CONFIG_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_CONFIG_H_

#include <cstddef>
#include <filesystem>
#include <string>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/websocket/types.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::gate {

struct OrderSessionCredentialsConfig {
  std::string api_key_env;
  std::string api_secret_env;
};

struct OrderSessionConfig {
  std::string name;
  websocket::ConnectionConfig connection;
  OrderSessionCredentialsConfig credentials;
  std::size_t request_map_capacity{kDefaultOrderRequestMapCapacity};
};

using OrderSessionConfigResult = Result<OrderSessionConfig>;

[[nodiscard]] OrderSessionConfigResult ParseOrderSessionConfig(
    const toml::table& node);

[[nodiscard]] OrderSessionConfigResult ParseOrderSessionConfig(
    const toml::table& node, const std::filesystem::path& config_file_path);

[[nodiscard]] OrderSessionConfigResult LoadOrderSessionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_CONFIG_H_
