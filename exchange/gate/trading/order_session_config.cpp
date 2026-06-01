#include "exchange/gate/trading/order_session_config.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/common/order_ack_diagnostic_level.h"
#include "core/config/websocket_config.h"
#include "exchange/gate/trading/decimal_size_header.h"
#include "nova/utils/log.h"

namespace aquila::gate {
namespace {

struct RawOrderSessionConfig {
  std::string name;
  std::string settle{"usdt"};
  OrderSessionCredentialsConfig credentials;
  config::WebSocketConfig websocket;
  std::size_t request_map_capacity{kDefaultOrderRequestMapCapacity};
  bool enable_tcp_info_diagnostics{false};
  OrderLatencyDiagnosticConfig ack_latency_diagnostics{};
  websocket::SocketTimestampingConfig socket_timestamping{};
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

  [[nodiscard]] std::int64_t NonNegativeInt64Or(
      toml::node_view<const toml::node> value_node, std::int64_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value < 0) {
      Fail(name, " must be non-negative");
      return fallback;
    }
    return *value;
  }

  [[nodiscard]] std::uint32_t NonNegativeUint32Or(
      toml::node_view<const toml::node> value_node, std::uint32_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value < 0 || *value > static_cast<std::int64_t>(
                                   std::numeric_limits<std::uint32_t>::max())) {
      Fail(name, " must fit uint32");
      return fallback;
    }
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] bool BoolOr(toml::node_view<const toml::node> value_node,
                            bool fallback) const {
    const std::optional<bool> value = value_node.value<bool>();
    return value.value_or(fallback);
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
    const toml::node_view<const toml::node> diagnostics =
        order_session["diagnostics"];
    config_.order_session.enable_tcp_info_diagnostics =
        BoolOr(diagnostics["enable_tcp_info"],
               config_.order_session.enable_tcp_info_diagnostics);
    OrderLatencyDiagnosticConfig& ack_latency =
        config_.order_session.ack_latency_diagnostics;
    ack_latency.ack_rtt_threshold_ns = NonNegativeInt64Or(
        diagnostics["ack_rtt_threshold_ns"], ack_latency.ack_rtt_threshold_ns,
        "order_session.diagnostics.ack_rtt_threshold_ns");
    if (!ok_) {
      return;
    }
    ack_latency.send_to_first_drive_read_threshold_ns =
        NonNegativeInt64Or(diagnostics["send_to_first_drive_read_threshold_ns"],
                           ack_latency.send_to_first_drive_read_threshold_ns,
                           "order_session.diagnostics."
                           "send_to_first_drive_read_threshold_ns");
    if (!ok_) {
      return;
    }
    ack_latency.drive_read_duration_threshold_ns = NonNegativeInt64Or(
        diagnostics["drive_read_duration_threshold_ns"],
        ack_latency.drive_read_duration_threshold_ns,
        "order_session.diagnostics.drive_read_duration_threshold_ns");
    if (!ok_) {
      return;
    }
    ack_latency.diagnostic_window_timeout_ns = NonNegativeInt64Or(
        diagnostics["diagnostic_window_timeout_ns"],
        ack_latency.diagnostic_window_timeout_ns,
        "order_session.diagnostics.diagnostic_window_timeout_ns");
    if (!ok_) {
      return;
    }
    ack_latency.max_logs_per_second = NonNegativeUint32Or(
        diagnostics["max_logs_per_second"], ack_latency.max_logs_per_second,
        "order_session.diagnostics.max_logs_per_second");
    if (!ok_) {
      return;
    }
    ParseSocketTimestampingConfig(diagnostics["timestamping"],
                                  &config_.order_session.socket_timestamping,
                                  "order_session.diagnostics.timestamping");
    if (!ok_) {
      return;
    }
    ValidateDiagnosticLevel();
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
    order_session_config.connection.socket_timestamping =
        config_.order_session.socket_timestamping;
    AddGateSizeDecimalHeader(order_session_config.connection);
    order_session_config.credentials =
        std::move(config_.order_session.credentials);
    order_session_config.request_map_capacity =
        config_.order_session.request_map_capacity;
    order_session_config.enable_tcp_info_diagnostics =
        config_.order_session.enable_tcp_info_diagnostics;
    order_session_config.ack_latency_diagnostics =
        config_.order_session.ack_latency_diagnostics;
    return Success(std::move(order_session_config));
  }

  void ParseSocketTimestampingConfig(
      toml::node_view<const toml::node> node,
      websocket::SocketTimestampingConfig* timestamping,
      std::string_view name) {
    timestamping->enabled = BoolOr(node["enabled"], timestamping->enabled);
    timestamping->tx_sched = BoolOr(node["tx_sched"], timestamping->tx_sched);
    timestamping->tx_software =
        BoolOr(node["tx_software"], timestamping->tx_software);
    timestamping->tx_ack = BoolOr(node["tx_ack"], timestamping->tx_ack);
    timestamping->rx_software =
        BoolOr(node["rx_software"], timestamping->rx_software);
    timestamping->hardware = BoolOr(node["hardware"], timestamping->hardware);
    timestamping->max_errqueue_events_per_drain = NonNegativeUint32Or(
        node["max_errqueue_events_per_drain"],
        timestamping->max_errqueue_events_per_drain,
        std::string{name}.append(".max_errqueue_events_per_drain"));
    timestamping->max_active_probes = NonNegativeUint32Or(
        node["max_active_probes"], timestamping->max_active_probes,
        std::string{name}.append(".max_active_probes"));
  }

  void ValidateDiagnosticLevel() {
    if (config_.order_session.enable_tcp_info_diagnostics &&
        !core::kOrderAckDiagnosticTcpInfoEnabled) {
      Fail("order_session.diagnostics.enable_tcp_info",
           " requires AQUILA_ORDER_ACK_DIAG_LEVEL >= 3");
      return;
    }
    if (config_.order_session.socket_timestamping.enabled &&
        !core::kOrderAckDiagnosticSocketTimestampingEnabled) {
      Fail("order_session.diagnostics.timestamping.enabled",
           " requires AQUILA_ORDER_ACK_DIAG_LEVEL >= 4");
    }
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
