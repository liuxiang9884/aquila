#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_FEEDBACK_SESSION_CONFIG_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_FEEDBACK_SESSION_CONFIG_H_

#include <filesystem>
#include <string>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/config/order_feedback_shm_config.h"
#include "core/websocket/types.h"
#include "exchange/bitget/trading/order_types.h"

namespace aquila::bitget {

struct OrderFeedbackSessionCredentialsConfig {
  std::string api_key_env;
  std::string api_secret_env;
  std::string api_passphrase_env;
};

struct OrderFeedbackSessionConfig {
  std::string name;
  std::string category{"usdt-futures"};
  std::string position_mode{"one_way_mode"};
  std::string margin_mode{"crossed"};
  ClientOidRunNamespace client_oid_run_namespace;
  websocket::ConnectionConfig connection;
  OrderFeedbackSessionCredentialsConfig credentials;
  config::OrderFeedbackShmRuntimeConfig shm;
};

using OrderFeedbackSessionConfigResult = Result<OrderFeedbackSessionConfig>;

[[nodiscard]] OrderFeedbackSessionConfigResult ParseOrderFeedbackSessionConfig(
    const toml::table& node);

[[nodiscard]] OrderFeedbackSessionConfigResult ParseOrderFeedbackSessionConfig(
    const toml::table& node, const std::filesystem::path& config_file_path);

[[nodiscard]] OrderFeedbackSessionConfigResult
LoadOrderFeedbackSessionConfigFile(const std::filesystem::path& path);

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_FEEDBACK_SESSION_CONFIG_H_
