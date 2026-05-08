#include "exchange/gate/trading/order_feedback_session_config.h"

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

namespace aquila::gate {
namespace {

struct RawOrderFeedbackSessionConfig {
  std::string name;
  std::string settle{"usdt"};
  OrderFeedbackSessionCredentialsConfig credentials;
  config::WebSocketConfig websocket;
  config::OrderFeedbackShmRuntimeConfig shm;
};

struct RawConfigFile {
  RawOrderFeedbackSessionConfig order_feedback_session;
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

[[nodiscard]] std::string BuildOrderFeedbackSessionTarget(
    std::string_view settle) {
  std::string target{"/v4/ws/"};
  target.append(settle);
  return target;
}

class OrderFeedbackSessionConfigParser {
 public:
  explicit OrderFeedbackSessionConfigParser(const toml::table& node)
      : node_(node) {}
  OrderFeedbackSessionConfigParser(const toml::table& node,
                                   std::filesystem::path config_file_path)
      : node_(node), config_file_path_(std::move(config_file_path)) {}

  [[nodiscard]] OrderFeedbackSessionConfigResult Parse() {
    ParseOrderFeedbackSession();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    config::WebSocketConfigResult websocket_result =
        config::ParseWebSocketConfig(
            node_["order_feedback_session"]["websocket"]);
    if (!websocket_result.ok) {
      return Failure(websocket_result.error);
    }
    config_.order_feedback_session.websocket =
        std::move(websocket_result.value);
    return BuildConfig();
  }

 private:
  [[nodiscard]] std::string StringOr(
      toml::node_view<const toml::node> value_node,
      const std::string& fallback) const {
    const std::optional<std::string> value = value_node.value<std::string>();
    return value.value_or(fallback);
  }

  [[nodiscard]] bool BoolOr(toml::node_view<const toml::node> value_node,
                            bool fallback) const {
    const std::optional<bool> value = value_node.value<bool>();
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

  [[nodiscard]] std::uint32_t UInt32Or(
      toml::node_view<const toml::node> value_node, std::uint32_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value <= 0) {
      Fail(name, " must be positive");
      return fallback;
    }
    if (*value > std::numeric_limits<std::uint32_t>::max()) {
      Fail(name, " exceeds uint32 range");
      return fallback;
    }
    return static_cast<std::uint32_t>(*value);
  }

  void ParseOrderFeedbackSession() {
    const toml::node_view<const toml::node> order_feedback_session =
        node_["order_feedback_session"];
    config_.order_feedback_session.name = RequiredString(
        order_feedback_session["name"], "order_feedback_session.name");
    if (!ok_) {
      return;
    }

    config_.order_feedback_session.settle =
        StringOr(order_feedback_session["settle"],
                 config_.order_feedback_session.settle);
    if (config_.order_feedback_session.settle.empty()) {
      Fail("order_feedback_session.settle", " must be non-empty");
      return;
    }

    ValidateOptionalTarget(order_feedback_session["websocket"]);
    if (!ok_) {
      return;
    }

    const toml::node_view<const toml::node> credentials =
        order_feedback_session["credentials"];
    config_.order_feedback_session.credentials.api_key_env =
        RequiredString(credentials["api_key_env"],
                       "order_feedback_session.credentials.api_key_env");
    if (!ok_) {
      return;
    }
    config_.order_feedback_session.credentials.api_secret_env =
        RequiredString(credentials["api_secret_env"],
                       "order_feedback_session.credentials.api_secret_env");
    if (!ok_) {
      return;
    }

    ParseShm(order_feedback_session["shm"]);
  }

  void ValidateOptionalTarget(toml::node_view<const toml::node> websocket) {
    const std::optional<std::string> target =
        websocket["target"].value<std::string>();
    if (!target) {
      return;
    }

    const std::string expected_target =
        BuildOrderFeedbackSessionTarget(config_.order_feedback_session.settle);
    if (*target != expected_target) {
      Fail("order_feedback_session.websocket.target",
           " must match /v4/ws/<settle>");
    }
  }

  void ParseShm(toml::node_view<const toml::node> shm) {
    config_.order_feedback_session.shm.shm_name =
        RequiredString(shm["shm_name"], "order_feedback_session.shm.shm_name");
    if (!ok_) {
      return;
    }
    config_.order_feedback_session.shm.channel_name = RequiredString(
        shm["channel_name"], "order_feedback_session.shm.channel_name");
    if (!ok_) {
      return;
    }

    config_.order_feedback_session.shm.max_strategy_count =
        UInt32Or(shm["max_strategy_count"],
                 config_.order_feedback_session.shm.max_strategy_count,
                 "order_feedback_session.shm.max_strategy_count");
    if (!ok_) {
      return;
    }
    if (config_.order_feedback_session.shm.max_strategy_count !=
        config::kOrderFeedbackShmMaxStrategyCount) {
      Fail("order_feedback_session.shm.max_strategy_count",
           " must equal compiled feedback lane count");
      return;
    }

    config_.order_feedback_session.shm.queue_capacity =
        UInt32Or(shm["queue_capacity"],
                 config_.order_feedback_session.shm.queue_capacity,
                 "order_feedback_session.shm.queue_capacity");
    if (!ok_) {
      return;
    }
    if (config_.order_feedback_session.shm.queue_capacity !=
        config::kOrderFeedbackShmQueueCapacity) {
      Fail("order_feedback_session.shm.queue_capacity",
           " must equal compiled queue capacity");
      return;
    }

    config_.order_feedback_session.shm.create =
        BoolOr(shm["create"], config_.order_feedback_session.shm.create);
    config_.order_feedback_session.shm.remove_existing =
        BoolOr(shm["remove_existing"],
               config_.order_feedback_session.shm.remove_existing);
    if (config_.order_feedback_session.shm.remove_existing &&
        !config_.order_feedback_session.shm.create) {
      Fail("order_feedback_session.shm.remove_existing",
           " requires create=true");
    }
  }

  [[nodiscard]] OrderFeedbackSessionConfigResult BuildConfig() {
    config::ConnectionConfigResult connection_result =
        config::ToConnectionConfig(config_.order_feedback_session.websocket,
                                   BuildOrderFeedbackSessionTarget(
                                       config_.order_feedback_session.settle));
    if (!connection_result.ok) {
      return Failure(connection_result.error);
    }

    OrderFeedbackSessionConfig session_config;
    session_config.name = std::move(config_.order_feedback_session.name);
    session_config.connection = std::move(connection_result.value);
    session_config.credentials =
        std::move(config_.order_feedback_session.credentials);
    session_config.shm = std::move(config_.order_feedback_session.shm);
    return Success(std::move(session_config));
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
  } catch (const std::exception& exc) {
    return Failure(
        std::string{"failed to load Gate order feedback session config: "} +
        exc.what());
  }
}

}  // namespace aquila::gate
