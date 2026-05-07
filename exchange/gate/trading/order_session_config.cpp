#include "exchange/gate/trading/order_session_config.h"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/config/websocket_config.h"
#include "nova/utils/log.h"

namespace aquila::gate {
namespace {

struct RawOrderSessionConfig {
  std::string name;
  std::string settle{"usdt"};
  OrderSessionCredentialsConfig credentials;
  config::WebSocketConfig websocket;
  std::size_t request_map_capacity{kDefaultOrderRequestMapCapacity};
};

struct RawConfigFile {
  RawOrderSessionConfig order_session;
};

void MaybeLogError(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_ERROR("{}", message);
  }
}

[[nodiscard]] OrderSessionConfigResult Failure(std::string error) {
  MaybeLogError(error);
  OrderSessionConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] OrderSessionConfigResult Success(OrderSessionConfig config) {
  OrderSessionConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

[[nodiscard]] std::string BuildOrderSessionTarget(std::string_view settle) {
  std::string target{"/v4/ws/"};
  target.append(settle);
  return target;
}

class OrderSessionConfigParser {
 public:
  explicit OrderSessionConfigParser(const toml::table& node) : node_(node) {}
  OrderSessionConfigParser(const toml::table& node,
                           std::filesystem::path config_file_path)
      : node_(node), config_file_path_(std::move(config_file_path)) {}

  [[nodiscard]] OrderSessionConfigResult Parse() {
    ParseOrderSession();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    config::WebSocketConfigResult websocket_result =
        config::ParseWebSocketConfig(node_["order_session"]["websocket"]);
    if (!websocket_result.ok) {
      return Failure(websocket_result.error);
    }
    config_.order_session.websocket = std::move(websocket_result.value);
    return BuildConfig();
  }

 private:
  [[nodiscard]] std::string StringOr(
      toml::node_view<const toml::node> value_node,
      const std::string& fallback) const {
    const std::optional<std::string> value = value_node.value<std::string>();
    return value.value_or(fallback);
  }

  [[nodiscard]] std::string RequiredString(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::string> value = value_node.value<std::string>();
    if (!value || value->empty()) {
      Fail(name, " is required");
      return {};
    }
    return *value;
  }

  [[nodiscard]] std::size_t SizeOr(toml::node_view<const toml::node> value_node,
                                   std::size_t fallback,
                                   std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value <= 0) {
      Fail(name, " must be positive");
      return fallback;
    }
    return static_cast<std::size_t>(*value);
  }

  void ParseOrderSession() {
    const toml::node_view<const toml::node> order_session =
        node_["order_session"];
    config_.order_session.name =
        RequiredString(order_session["name"], "order_session.name");
    if (!ok_) {
      return;
    }
    config_.order_session.settle =
        StringOr(order_session["settle"], config_.order_session.settle);
    if (config_.order_session.settle.empty()) {
      Fail("order_session.settle", " must be non-empty");
      return;
    }
    config_.order_session.request_map_capacity =
        SizeOr(order_session["request_map_capacity"],
               config_.order_session.request_map_capacity,
               "order_session.request_map_capacity");
    if (!ok_) {
      return;
    }

    const toml::node_view<const toml::node> credentials =
        order_session["credentials"];
    config_.order_session.credentials.api_key_env = RequiredString(
        credentials["api_key_env"], "order_session.credentials.api_key_env");
    if (!ok_) {
      return;
    }
    config_.order_session.credentials.api_secret_env =
        RequiredString(credentials["api_secret_env"],
                       "order_session.credentials.api_secret_env");
  }

  [[nodiscard]] OrderSessionConfigResult BuildConfig() {
    config::ConnectionConfigResult connection_result =
        config::ToConnectionConfig(
            config_.order_session.websocket,
            BuildOrderSessionTarget(config_.order_session.settle));
    if (!connection_result.ok) {
      return Failure(connection_result.error);
    }

    OrderSessionConfig order_session_config;
    order_session_config.name = std::move(config_.order_session.name);
    order_session_config.connection = std::move(connection_result.value);
    order_session_config.credentials =
        std::move(config_.order_session.credentials);
    order_session_config.request_map_capacity =
        config_.order_session.request_map_capacity;
    return Success(std::move(order_session_config));
  }

  void Fail(std::string_view name, std::string_view message) {
    ok_ = false;
    error_.assign(name);
    error_.append(message);
  }

  const toml::table& node_;
  [[maybe_unused]] std::filesystem::path config_file_path_;
  RawConfigFile config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

OrderSessionConfigResult ParseOrderSessionConfig(const toml::table& node) {
  return OrderSessionConfigParser{node}.Parse();
}

OrderSessionConfigResult ParseOrderSessionConfig(
    const toml::table& node, const std::filesystem::path& config_file_path) {
  return OrderSessionConfigParser{node, config_file_path}.Parse();
}

OrderSessionConfigResult LoadOrderSessionConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseOrderSessionConfig(parsed, path);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load Gate order session config: "} +
                   exc.what());
  }
}

}  // namespace aquila::gate
