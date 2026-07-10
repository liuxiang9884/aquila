#include "exchange/bitget/trading/order_feedback_session_config.h"

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

struct RawOrderFeedbackSessionConfig {
  std::string name;
  std::string category{"usdt-futures"};
  std::string position_mode{"one_way_mode"};
  std::string margin_mode{"crossed"};
  std::string target;
  OrderFeedbackSessionCredentialsConfig credentials;
  config::WebSocketConfig websocket;
  config::OrderFeedbackShmRuntimeConfig shm;
};

void MaybeLogError(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_ERROR("{}", message);
  }
}

[[nodiscard]] OrderFeedbackSessionConfigResult Failure(std::string error) {
  MaybeLogError(error);
  OrderFeedbackSessionConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] OrderFeedbackSessionConfigResult Success(
    OrderFeedbackSessionConfig config) {
  OrderFeedbackSessionConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

class OrderFeedbackSessionConfigParser {
 public:
  explicit OrderFeedbackSessionConfigParser(const toml::table& node)
      : node_(node) {}

  OrderFeedbackSessionConfigParser(const toml::table& node,
                                   std::filesystem::path config_file_path)
      : node_(node), config_file_path_(std::move(config_file_path)) {}

  [[nodiscard]] OrderFeedbackSessionConfigResult Parse() {
    ParseSession();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    config::WebSocketConfigResult websocket_result =
        config::ParseWebSocketConfig(
            node_["order_feedback_session"]["websocket"]);
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

  [[nodiscard]] bool BoolOr(toml::node_view<const toml::node> value_node,
                            bool fallback) const {
    return value_node.value<bool>().value_or(fallback);
  }

  [[nodiscard]] std::uint32_t UInt32Or(
      toml::node_view<const toml::node> value_node, std::uint32_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value <= 0 || static_cast<std::uint64_t>(*value) >
                           std::numeric_limits<std::uint32_t>::max()) {
      Fail(name, " must be a positive uint32");
      return fallback;
    }
    return static_cast<std::uint32_t>(*value);
  }

  void ParseSession() {
    const toml::node_view<const toml::node> session =
        node_["order_feedback_session"];
    raw_.name = RequiredString(session["name"], "order_feedback_session.name");
    if (!ok_) {
      return;
    }
    raw_.category =
        RequiredString(session["category"], "order_feedback_session.category");
    if (!ok_) {
      return;
    }
    raw_.position_mode = RequiredString(session["position_mode"],
                                        "order_feedback_session.position_mode");
    if (!ok_) {
      return;
    }
    raw_.margin_mode = RequiredString(session["margin_mode"],
                                      "order_feedback_session.margin_mode");
    if (!ok_) {
      return;
    }
    if (raw_.category != "usdt-futures") {
      Fail("order_feedback_session.category", " must be usdt-futures");
      return;
    }
    if (raw_.position_mode != "one_way_mode") {
      Fail("order_feedback_session.position_mode", " must be one_way_mode");
      return;
    }
    if (raw_.margin_mode != "crossed") {
      Fail("order_feedback_session.margin_mode", " must be crossed");
      return;
    }

    const toml::node_view<const toml::node> credentials =
        session["credentials"];
    raw_.credentials.api_key_env =
        RequiredString(credentials["api_key_env"],
                       "order_feedback_session.credentials.api_key_env");
    if (!ok_) {
      return;
    }
    raw_.credentials.api_secret_env =
        RequiredString(credentials["api_secret_env"],
                       "order_feedback_session.credentials.api_secret_env");
    if (!ok_) {
      return;
    }
    raw_.credentials.api_passphrase_env =
        RequiredString(credentials["api_passphrase_env"],
                       "order_feedback_session.credentials.api_passphrase_env");
    if (!ok_) {
      return;
    }

    raw_.target =
        RequiredString(session["websocket"]["endpoint"]["target"],
                       "order_feedback_session.websocket.endpoint.target");
    if (!ok_) {
      return;
    }
    ParseShm(session["shm"]);
  }

  void ParseShm(toml::node_view<const toml::node> shm) {
    raw_.shm.shm_name =
        RequiredString(shm["shm_name"], "order_feedback_session.shm.shm_name");
    if (!ok_) {
      return;
    }
    raw_.shm.channel_name = RequiredString(
        shm["channel_name"], "order_feedback_session.shm.channel_name");
    if (!ok_) {
      return;
    }
    raw_.shm.max_strategy_count =
        UInt32Or(shm["max_strategy_count"], raw_.shm.max_strategy_count,
                 "order_feedback_session.shm.max_strategy_count");
    if (!ok_) {
      return;
    }
    if (raw_.shm.max_strategy_count !=
        config::kOrderFeedbackShmMaxStrategyCount) {
      Fail("order_feedback_session.shm.max_strategy_count",
           " must equal compiled feedback lane count");
      return;
    }
    raw_.shm.queue_capacity =
        UInt32Or(shm["queue_capacity"], raw_.shm.queue_capacity,
                 "order_feedback_session.shm.queue_capacity");
    if (!ok_) {
      return;
    }
    if (raw_.shm.queue_capacity != config::kOrderFeedbackShmQueueCapacity) {
      Fail("order_feedback_session.shm.queue_capacity",
           " must equal compiled queue capacity");
      return;
    }
    raw_.shm.create = BoolOr(shm["create"], raw_.shm.create);
    raw_.shm.remove_existing =
        BoolOr(shm["remove_existing"], raw_.shm.remove_existing);
    if (raw_.shm.remove_existing && !raw_.shm.create) {
      Fail("order_feedback_session.shm.remove_existing",
           " requires create=true");
    }
  }

  void ValidateWebSocket() {
    if (raw_.target != "/v3/ws/private") {
      Fail("order_feedback_session.websocket.endpoint.target",
           " must be /v3/ws/private");
      return;
    }
    if (!raw_.websocket.endpoint.enable_tls ||
        raw_.websocket.endpoint.port != "443") {
      Fail("order_feedback_session.websocket.endpoint",
           " must use TLS on port 443");
      return;
    }
    if (raw_.websocket.heartbeat.interval_ms == 0 ||
        raw_.websocket.heartbeat.interval_ms > 30'000) {
      Fail("order_feedback_session.websocket.heartbeat.interval_ms",
           " must be in [1, 30000]");
      return;
    }
    if (raw_.websocket.heartbeat.timeout_ms == 0) {
      Fail("order_feedback_session.websocket.heartbeat.timeout_ms",
           " must be positive");
    }
  }

  [[nodiscard]] OrderFeedbackSessionConfigResult BuildConfig() {
    config::ConnectionConfigResult connection_result =
        config::ToConnectionConfig(raw_.websocket, raw_.target);
    if (!connection_result.ok) {
      return Failure(connection_result.error);
    }

    OrderFeedbackSessionConfig config;
    config.name = std::move(raw_.name);
    config.category = std::move(raw_.category);
    config.position_mode = std::move(raw_.position_mode);
    config.margin_mode = std::move(raw_.margin_mode);
    config.connection = std::move(connection_result.value);
    config.credentials = std::move(raw_.credentials);
    config.shm = std::move(raw_.shm);
    return Success(std::move(config));
  }

  void Fail(std::string_view name, std::string_view message) {
    ok_ = false;
    error_.assign(name);
    error_.append(message);
  }

  const toml::table& node_;
  [[maybe_unused]] std::filesystem::path config_file_path_;
  RawOrderFeedbackSessionConfig raw_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

OrderFeedbackSessionConfigResult ParseOrderFeedbackSessionConfig(
    const toml::table& node) {
  return OrderFeedbackSessionConfigParser{node}.Parse();
}

OrderFeedbackSessionConfigResult ParseOrderFeedbackSessionConfig(
    const toml::table& node, const std::filesystem::path& config_file_path) {
  return OrderFeedbackSessionConfigParser{node, config_file_path}.Parse();
}

OrderFeedbackSessionConfigResult LoadOrderFeedbackSessionConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseOrderFeedbackSessionConfig(parsed, path);
  } catch (const std::exception& exception) {
    return Failure(
        std::string{"failed to load Bitget order feedback session config: "} +
        exception.what());
  }
}

}  // namespace aquila::bitget
