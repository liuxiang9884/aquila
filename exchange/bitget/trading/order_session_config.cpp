#include "exchange/bitget/trading/order_session_config.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/config/websocket_config.h"
#include "nova/utils/log.h"

namespace aquila::bitget {
namespace {

struct RawOrderSessionConfig {
  std::string name;
  std::string category{"usdt-futures"};
  std::string position_mode{"one_way_mode"};
  std::string margin_mode{"crossed"};
  std::string target;
  OrderSessionCredentialsConfig credentials;
  config::WebSocketConfig websocket;
  std::size_t request_map_capacity{kDefaultOrderRequestMapCapacity};
  std::size_t order_id_cache_capacity{kDefaultOrderIdCacheCapacity};
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
    raw_.websocket = std::move(websocket_result.value);
    ValidateWebSocket();
    if (!ok_) {
      return Failure(std::move(error_));
    }
    return BuildConfig();
  }

 private:
  [[nodiscard]] std::string RequiredString(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::string> value = value_node.value<std::string>();
    if (!value || value->empty()) {
      Fail(name, " is required");
      return {};
    }
    return *value;
  }

  [[nodiscard]] std::size_t RequiredSize(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value || *value <= 0 ||
        static_cast<std::uint64_t>(*value) >
            std::numeric_limits<std::size_t>::max()) {
      Fail(name, " must be a positive size");
      return 0;
    }
    return static_cast<std::size_t>(*value);
  }

  void ParseOrderSession() {
    const toml::node_view<const toml::node> order_session =
        node_["order_session"];
    raw_.name = RequiredString(order_session["name"], "order_session.name");
    if (!ok_) {
      return;
    }
    raw_.category =
        RequiredString(order_session["category"], "order_session.category");
    if (!ok_) {
      return;
    }
    raw_.position_mode = RequiredString(order_session["position_mode"],
                                        "order_session.position_mode");
    if (!ok_) {
      return;
    }
    raw_.margin_mode = RequiredString(order_session["margin_mode"],
                                      "order_session.margin_mode");
    if (!ok_) {
      return;
    }
    if (raw_.category != "usdt-futures") {
      Fail("order_session.category", " must be usdt-futures");
      return;
    }
    if (raw_.position_mode != "one_way_mode") {
      Fail("order_session.position_mode", " must be one_way_mode");
      return;
    }
    if (raw_.margin_mode != "crossed") {
      Fail("order_session.margin_mode", " must be crossed");
      return;
    }

    raw_.request_map_capacity =
        RequiredSize(order_session["request_map_capacity"],
                     "order_session.request_map_capacity");
    if (!ok_) {
      return;
    }
    raw_.order_id_cache_capacity =
        RequiredSize(order_session["order_id_cache_capacity"],
                     "order_session.order_id_cache_capacity");
    if (!ok_) {
      return;
    }

    const toml::node_view<const toml::node> credentials =
        order_session["credentials"];
    raw_.credentials.api_key_env = RequiredString(
        credentials["api_key_env"], "order_session.credentials.api_key_env");
    if (!ok_) {
      return;
    }
    raw_.credentials.api_secret_env =
        RequiredString(credentials["api_secret_env"],
                       "order_session.credentials.api_secret_env");
    if (!ok_) {
      return;
    }
    raw_.credentials.api_passphrase_env =
        RequiredString(credentials["api_passphrase_env"],
                       "order_session.credentials.api_passphrase_env");
    if (!ok_) {
      return;
    }
    raw_.target =
        RequiredString(order_session["websocket"]["endpoint"]["target"],
                       "order_session.websocket.endpoint.target");
  }

  void ValidateWebSocket() {
    if (raw_.target != "/v3/ws/private") {
      Fail("order_session.websocket.endpoint.target",
           " must be /v3/ws/private");
      return;
    }
    if (!raw_.websocket.endpoint.enable_tls ||
        raw_.websocket.endpoint.port != "443") {
      Fail("order_session.websocket.endpoint", " must use TLS on port 443");
      return;
    }
    if (raw_.websocket.heartbeat.interval_ms == 0 ||
        raw_.websocket.heartbeat.interval_ms > 30'000) {
      Fail("order_session.websocket.heartbeat.interval_ms",
           " must be in [1, 30000]");
      return;
    }
    if (raw_.websocket.heartbeat.timeout_ms == 0) {
      Fail("order_session.websocket.heartbeat.timeout_ms", " must be positive");
    }
  }

  [[nodiscard]] OrderSessionConfigResult BuildConfig() {
    config::ConnectionConfigResult connection_result =
        config::ToConnectionConfig(raw_.websocket, raw_.target);
    if (!connection_result.ok) {
      return Failure(connection_result.error);
    }
    OrderSessionConfig config;
    config.name = std::move(raw_.name);
    config.category = std::move(raw_.category);
    config.position_mode = std::move(raw_.position_mode);
    config.margin_mode = std::move(raw_.margin_mode);
    config.connection = std::move(connection_result.value);
    config.credentials = std::move(raw_.credentials);
    config.request_map_capacity = raw_.request_map_capacity;
    config.order_id_cache_capacity = raw_.order_id_cache_capacity;
    return Success(std::move(config));
  }

  void Fail(std::string_view name, std::string_view message) {
    ok_ = false;
    error_.assign(name);
    error_.append(message);
  }

  const toml::table& node_;
  [[maybe_unused]] std::filesystem::path config_file_path_;
  RawOrderSessionConfig raw_;
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
  } catch (const std::exception& exception) {
    return Failure(std::string{"failed to load Bitget order session config: "} +
                   exception.what());
  }
}

}  // namespace aquila::bitget
