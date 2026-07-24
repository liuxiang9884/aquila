#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SESSION_CONFIG_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SESSION_CONFIG_H_

#include <cstddef>
#include <filesystem>
#include <string>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/websocket/types.h"
#include "exchange/bitget/trading/order_types.h"

namespace aquila::bitget {

struct OrderSessionCredentialsConfig {
  std::string api_key_env;
  std::string api_secret_env;
  std::string api_passphrase_env;
};

struct OrderSessionConfig {
  std::string name;
  std::string category{"usdt-futures"};
  std::string position_mode{"one_way_mode"};
  std::string margin_mode{"crossed"};
  ClientOidRunNamespace client_oid_run_namespace;
  websocket::ConnectionConfig connection;
  OrderSessionCredentialsConfig credentials;
  std::size_t request_map_capacity{kDefaultOrderRequestMapCapacity};
  std::size_t order_id_cache_capacity{kDefaultOrderIdCacheCapacity};
};

using OrderSessionConfigResult = Result<OrderSessionConfig>;

[[nodiscard]] OrderSessionConfigResult ParseOrderSessionConfig(
    const toml::table& node);

[[nodiscard]] OrderSessionConfigResult ParseOrderSessionConfig(
    const toml::table& node, const std::filesystem::path& config_file_path);

[[nodiscard]] OrderSessionConfigResult LoadOrderSessionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SESSION_CONFIG_H_
