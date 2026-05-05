#include "core/config/websocket_config.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "nova/utils/log.h"

namespace aquila::config {
namespace {

void MaybeLogConfigError(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_ERROR("{}", message);
  }
}

[[nodiscard]] WebSocketConfigResult ConfigFailure(std::string message) {
  MaybeLogConfigError(message);
  WebSocketConfigResult result;
  result.error = std::move(message);
  return result;
}

[[nodiscard]] WebSocketConfigResult ConfigSuccess(WebSocketConfig config) {
  WebSocketConfigResult result;
  result.config = std::move(config);
  result.ok = true;
  return result;
}

[[nodiscard]] ConnectionConfigResult ConnectionFailure(std::string message) {
  MaybeLogConfigError(message);
  ConnectionConfigResult result;
  result.error = std::move(message);
  return result;
}

[[nodiscard]] ConnectionConfigResult ConnectionSuccess(
    websocket::ConnectionConfig config) {
  ConnectionConfigResult result;
  result.config = std::move(config);
  result.ok = true;
  return result;
}

[[nodiscard]] std::optional<websocket::AffinityMode> ParseAffinityMode(
    std::string_view text) {
  if (text == "none") {
    return websocket::AffinityMode::kNone;
  }
  if (text == "best_effort") {
    return websocket::AffinityMode::kBestEffort;
  }
  if (text == "required") {
    return websocket::AffinityMode::kRequired;
  }
  return std::nullopt;
}

class WebSocketConfigParser {
 public:
  explicit WebSocketConfigParser(toml::node_view<const toml::node> node)
      : node_(node) {}

  [[nodiscard]] WebSocketConfigResult Parse() {
    ParseEndpoint();
    if (!ok_) {
      return ConfigFailure(std::move(error_));
    }

    ParseExecutionPolicy();
    if (!ok_) {
      return ConfigFailure(std::move(error_));
    }

    ParseReadPath();
    ParseHeartbeat();
    ParseReconnect();
    return ConfigSuccess(std::move(config_));
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

  [[nodiscard]] std::uint32_t UInt32Or(
      toml::node_view<const toml::node> value_node,
      std::uint32_t fallback) const {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    return static_cast<std::uint32_t>(*value);
  }

  void ParseEndpoint() {
    const toml::node_view<const toml::node> endpoint = node_["endpoint"];
    const std::optional<std::string> host =
        endpoint["host"].value<std::string>();
    if (!host || host->empty()) {
      Fail("endpoint.host", " is required");
      return;
    }
    config_.endpoint.host = *host;

    config_.endpoint.service =
        StringOr(endpoint["service"], config_.endpoint.service);
    config_.endpoint.enable_tls =
        BoolOr(endpoint["enable_tls"], config_.endpoint.enable_tls);
    config_.endpoint.connect_timeout_ms = UInt32Or(
        endpoint["connect_timeout_ms"], config_.endpoint.connect_timeout_ms);
  }

  void ParseExecutionPolicy() {
    const toml::node_view<const toml::node> execution_policy =
        node_["execution_policy"];
    const std::optional<std::int64_t> bind_cpu_id =
        execution_policy["bind_cpu_id"].value<std::int64_t>();
    if (!bind_cpu_id) {
      Fail("execution_policy.bind_cpu_id", " is required");
      return;
    }
    config_.execution_policy.bind_cpu_id = static_cast<int>(*bind_cpu_id);

    const std::optional<std::string> affinity_mode =
        execution_policy["affinity_mode"].value<std::string>();
    if (affinity_mode) {
      const std::optional<websocket::AffinityMode> parsed =
          ParseAffinityMode(*affinity_mode);
      if (!parsed) {
        Fail("execution_policy.affinity_mode",
             " must be one of none, best_effort, required");
        return;
      }
      config_.execution_policy.affinity_mode = *parsed;
    }

    config_.execution_policy.lock_memory =
        BoolOr(execution_policy["lock_memory"],
               config_.execution_policy.lock_memory);
    config_.execution_policy.prefault_stack =
        BoolOr(execution_policy["prefault_stack"],
               config_.execution_policy.prefault_stack);
    config_.execution_policy.active_spin =
        BoolOr(execution_policy["active_spin"],
               config_.execution_policy.active_spin);
    config_.execution_policy.spin_iterations_before_clock_check =
        UInt32Or(execution_policy["spin_iterations_before_clock_check"],
                 config_.execution_policy.spin_iterations_before_clock_check);
  }

  void ParseReadPath() {
    const toml::node_view<const toml::node> read_path = node_["read_path"];
    config_.read_path.max_reads_per_drive =
        UInt32Or(read_path["max_reads_per_drive"],
                 config_.read_path.max_reads_per_drive);
    config_.read_path.read_until_would_block =
        BoolOr(read_path["read_until_would_block"],
               config_.read_path.read_until_would_block);
  }

  void ParseHeartbeat() {
    const toml::node_view<const toml::node> heartbeat = node_["heartbeat"];
    config_.heartbeat.interval_ms =
        UInt32Or(heartbeat["interval_ms"], config_.heartbeat.interval_ms);
    config_.heartbeat.timeout_ms =
        UInt32Or(heartbeat["timeout_ms"], config_.heartbeat.timeout_ms);
  }

  void ParseReconnect() {
    const toml::node_view<const toml::node> reconnect = node_["reconnect"];
    config_.reconnect.enabled =
        BoolOr(reconnect["enabled"], config_.reconnect.enabled);
    config_.reconnect.initial_backoff_ms =
        UInt32Or(reconnect["initial_backoff_ms"],
                 config_.reconnect.initial_backoff_ms);
    config_.reconnect.max_backoff_ms =
        UInt32Or(reconnect["max_backoff_ms"],
                 config_.reconnect.max_backoff_ms);
    config_.reconnect.backoff_shift_bits =
        UInt32Or(reconnect["backoff_shift_bits"],
                 config_.reconnect.backoff_shift_bits);
    config_.reconnect.jitter_percent =
        UInt32Or(reconnect["jitter_percent"], config_.reconnect.jitter_percent);
    config_.reconnect.max_attempts =
        UInt32Or(reconnect["max_attempts"], config_.reconnect.max_attempts);
  }

  void Fail(std::string_view name, std::string_view message) {
    ok_ = false;
    error_.assign(name);
    error_.append(message);
  }

  toml::node_view<const toml::node> node_;
  WebSocketConfig config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

WebSocketConfigResult ParseWebSocketConfig(
    toml::node_view<const toml::node> node) {
  return WebSocketConfigParser{node}.Parse();
}

ConnectionConfigResult ToConnectionConfig(const WebSocketConfig& config,
                                          std::string target) {
  if (target.empty()) {
    return ConnectionFailure("websocket target must be a non-empty string");
  }

  websocket::ConnectionConfig connection;
  connection.host = config.endpoint.host;
  connection.service = config.endpoint.service;
  connection.target = std::move(target);
  connection.enable_tls = config.endpoint.enable_tls;
  connection.cold_path_total_timeout_ms = config.endpoint.connect_timeout_ms;
  connection.max_reads_per_drive = config.read_path.max_reads_per_drive;
  connection.read_until_would_block = config.read_path.read_until_would_block;
  connection.heartbeat_interval_ms = config.heartbeat.interval_ms;
  connection.heartbeat_timeout_ms = config.heartbeat.timeout_ms;
  connection.reconnect.enabled = config.reconnect.enabled;
  connection.reconnect.initial_backoff_ms = config.reconnect.initial_backoff_ms;
  connection.reconnect.max_backoff_ms = config.reconnect.max_backoff_ms;
  connection.reconnect.backoff_shift_bits =
      static_cast<std::uint8_t>(config.reconnect.backoff_shift_bits);
  connection.reconnect.jitter_percent =
      static_cast<std::uint8_t>(config.reconnect.jitter_percent);
  connection.reconnect.max_attempts = config.reconnect.max_attempts;
  connection.runtime_policy.io_cpu_id = config.execution_policy.bind_cpu_id;
  connection.runtime_policy.affinity_mode =
      config.execution_policy.affinity_mode;
  connection.runtime_policy.lock_memory = config.execution_policy.lock_memory;
  connection.runtime_policy.prefault_stack =
      config.execution_policy.prefault_stack;
  connection.runtime_policy.active_spin = config.execution_policy.active_spin;
  connection.runtime_policy.spin_iterations_before_clock_check =
      config.execution_policy.spin_iterations_before_clock_check;
  return ConnectionSuccess(std::move(connection));
}

}  // namespace aquila::config
