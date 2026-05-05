#include "core/config/websocket_config.h"

#include <limits>
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

[[nodiscard]] std::string FieldName(std::string_view section,
                                    std::string_view key) {
  std::string name;
  name.reserve(section.size() + key.size() + 1);
  name.append(section);
  name.push_back('.');
  name.append(key);
  return name;
}

[[nodiscard]] bool RequireTable(toml::node_view<const toml::node> node,
                                std::string_view name, std::string* error) {
  if (!node || node.as_table() == nullptr) {
    *error = std::string{name} + " table is required";
    return false;
  }
  return true;
}

[[nodiscard]] bool ReadRequiredString(toml::node_view<const toml::node> node,
                                      std::string_view section,
                                      std::string_view key, std::string* output,
                                      std::string* error) {
  const toml::node_view<const toml::node> value_node = node[key];
  const std::string name = FieldName(section, key);
  if (!value_node) {
    *error = name + " is required";
    return false;
  }
  const std::optional<std::string> value = value_node.value<std::string>();
  if (!value || value->empty()) {
    *error = name + " must be a non-empty string";
    return false;
  }
  *output = *value;
  return true;
}

[[nodiscard]] bool ReadOptionalString(toml::node_view<const toml::node> node,
                                      std::string_view section,
                                      std::string_view key, std::string* output,
                                      std::string* error) {
  const toml::node_view<const toml::node> value_node = node[key];
  if (!value_node) {
    return true;
  }
  const std::string name = FieldName(section, key);
  const std::optional<std::string> value = value_node.value<std::string>();
  if (!value || value->empty()) {
    *error = name + " must be a non-empty string";
    return false;
  }
  *output = *value;
  return true;
}

[[nodiscard]] bool ReadOptionalBool(toml::node_view<const toml::node> node,
                                    std::string_view section,
                                    std::string_view key, bool* output,
                                    std::string* error) {
  const toml::node_view<const toml::node> value_node = node[key];
  if (!value_node) {
    return true;
  }
  const std::string name = FieldName(section, key);
  const std::optional<bool> value = value_node.value<bool>();
  if (!value) {
    *error = name + " must be a bool";
    return false;
  }
  *output = *value;
  return true;
}

[[nodiscard]] bool ReadOptionalUInt32(
    toml::node_view<const toml::node> node, std::string_view section,
    std::string_view key, std::uint32_t min_value, std::uint32_t max_value,
    std::uint32_t* output, std::string* error) {
  const toml::node_view<const toml::node> value_node = node[key];
  if (!value_node) {
    return true;
  }
  const std::string name = FieldName(section, key);
  const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
  if (!value) {
    *error = name + " must be an integer";
    return false;
  }
  if (*value < static_cast<std::int64_t>(min_value) ||
      *value > static_cast<std::int64_t>(max_value)) {
    *error = name + " is out of range";
    return false;
  }
  *output = static_cast<std::uint32_t>(*value);
  return true;
}

[[nodiscard]] bool ReadRequiredInt(toml::node_view<const toml::node> node,
                                   std::string_view section,
                                   std::string_view key, int min_value,
                                   int max_value, int* output,
                                   std::string* error) {
  const toml::node_view<const toml::node> value_node = node[key];
  const std::string name = FieldName(section, key);
  if (!value_node) {
    *error = name + " is required";
    return false;
  }
  const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
  if (!value) {
    *error = name + " must be an integer";
    return false;
  }
  if (*value < min_value || *value > max_value) {
    *error = name + " is out of range";
    return false;
  }
  *output = static_cast<int>(*value);
  return true;
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

[[nodiscard]] bool ReadOptionalAffinityMode(
    toml::node_view<const toml::node> node, std::string_view section,
    std::string_view key, websocket::AffinityMode* output, std::string* error) {
  const toml::node_view<const toml::node> value_node = node[key];
  if (!value_node) {
    return true;
  }
  const std::string name = FieldName(section, key);
  const std::optional<std::string> value = value_node.value<std::string>();
  if (!value) {
    *error = name + " must be a string";
    return false;
  }
  const std::optional<websocket::AffinityMode> mode = ParseAffinityMode(*value);
  if (!mode) {
    *error = name + " must be one of none, best_effort, required";
    return false;
  }
  *output = *mode;
  return true;
}

[[nodiscard]] bool ParseEndpoint(toml::node_view<const toml::node> node,
                                 WebSocketEndpointConfig* config,
                                 std::string* error) {
  constexpr std::string_view kSection = "endpoint";
  if (!ReadRequiredString(node, kSection, "host", &config->host, error)) {
    return false;
  }
  if (!ReadOptionalString(node, kSection, "service", &config->service, error)) {
    return false;
  }
  if (!ReadOptionalBool(node, kSection, "enable_tls", &config->enable_tls,
                        error)) {
    return false;
  }
  return ReadOptionalUInt32(node, kSection, "connect_timeout_ms", 1,
                            std::numeric_limits<std::uint32_t>::max(),
                            &config->connect_timeout_ms, error);
}

[[nodiscard]] bool ParseExecutionPolicy(toml::node_view<const toml::node> node,
                                        WebSocketExecutionPolicyConfig* config,
                                        std::string* error) {
  constexpr std::string_view kSection = "execution_policy";
  if (!ReadRequiredInt(node, kSection, "bind_cpu_id", 0,
                       std::numeric_limits<int>::max(), &config->bind_cpu_id,
                       error)) {
    return false;
  }
  if (!ReadOptionalAffinityMode(node, kSection, "affinity_mode",
                                &config->affinity_mode, error)) {
    return false;
  }
  if (!ReadOptionalBool(node, kSection, "lock_memory", &config->lock_memory,
                        error)) {
    return false;
  }
  if (!ReadOptionalBool(node, kSection, "prefault_stack",
                        &config->prefault_stack, error)) {
    return false;
  }
  if (!ReadOptionalBool(node, kSection, "active_spin", &config->active_spin,
                        error)) {
    return false;
  }
  return ReadOptionalUInt32(node, kSection,
                            "spin_iterations_before_clock_check", 1,
                            std::numeric_limits<std::uint32_t>::max(),
                            &config->spin_iterations_before_clock_check, error);
}

[[nodiscard]] bool ParseReadPath(toml::node_view<const toml::node> node,
                                 WebSocketReadPathConfig* config,
                                 std::string* error) {
  constexpr std::string_view kSection = "read_path";
  if (!ReadOptionalUInt32(node, kSection, "max_reads_per_drive", 1,
                          std::numeric_limits<std::uint32_t>::max(),
                          &config->max_reads_per_drive, error)) {
    return false;
  }
  return ReadOptionalBool(node, kSection, "read_until_would_block",
                          &config->read_until_would_block, error);
}

[[nodiscard]] bool ParseHeartbeat(toml::node_view<const toml::node> node,
                                  WebSocketHeartbeatConfig* config,
                                  std::string* error) {
  constexpr std::string_view kSection = "heartbeat";
  if (!ReadOptionalUInt32(node, kSection, "interval_ms", 1,
                          std::numeric_limits<std::uint32_t>::max(),
                          &config->interval_ms, error)) {
    return false;
  }
  if (!ReadOptionalUInt32(node, kSection, "timeout_ms", 1,
                          std::numeric_limits<std::uint32_t>::max(),
                          &config->timeout_ms, error)) {
    return false;
  }
  if (config->timeout_ms <= config->interval_ms) {
    *error = "heartbeat.timeout_ms must be greater than heartbeat.interval_ms";
    return false;
  }
  return true;
}

[[nodiscard]] bool ParseReconnect(toml::node_view<const toml::node> node,
                                  WebSocketReconnectConfig* config,
                                  std::string* error) {
  constexpr std::string_view kSection = "reconnect";
  if (!ReadOptionalBool(node, kSection, "enabled", &config->enabled, error)) {
    return false;
  }
  if (!ReadOptionalUInt32(node, kSection, "initial_backoff_ms", 1,
                          std::numeric_limits<std::uint32_t>::max(),
                          &config->initial_backoff_ms, error)) {
    return false;
  }
  if (!ReadOptionalUInt32(node, kSection, "max_backoff_ms", 1,
                          std::numeric_limits<std::uint32_t>::max(),
                          &config->max_backoff_ms, error)) {
    return false;
  }
  if (!ReadOptionalUInt32(node, kSection, "backoff_shift_bits", 0, 31,
                          &config->backoff_shift_bits, error)) {
    return false;
  }
  if (!ReadOptionalUInt32(node, kSection, "jitter_percent", 0, 100,
                          &config->jitter_percent, error)) {
    return false;
  }
  if (!ReadOptionalUInt32(node, kSection, "max_attempts", 0,
                          std::numeric_limits<std::uint32_t>::max(),
                          &config->max_attempts, error)) {
    return false;
  }
  if (config->max_backoff_ms < config->initial_backoff_ms) {
    *error = "reconnect.max_backoff_ms must be >= reconnect.initial_backoff_ms";
    return false;
  }
  return true;
}

[[nodiscard]] bool ValidateWebSocketConfig(const WebSocketConfig& config,
                                           std::string* error) {
  if (config.endpoint.host.empty()) {
    *error = "endpoint.host is required";
    return false;
  }
  if (config.endpoint.service.empty()) {
    *error = "endpoint.service must be a non-empty string";
    return false;
  }
  if (config.endpoint.connect_timeout_ms == 0) {
    *error = "endpoint.connect_timeout_ms must be positive";
    return false;
  }
  if (config.execution_policy.bind_cpu_id < 0) {
    *error = "execution_policy.bind_cpu_id is required";
    return false;
  }
  if (config.execution_policy.spin_iterations_before_clock_check == 0) {
    *error =
        "execution_policy.spin_iterations_before_clock_check must be positive";
    return false;
  }
  if (config.read_path.max_reads_per_drive == 0) {
    *error = "read_path.max_reads_per_drive must be positive";
    return false;
  }
  if (config.heartbeat.interval_ms == 0 || config.heartbeat.timeout_ms == 0 ||
      config.heartbeat.timeout_ms <= config.heartbeat.interval_ms) {
    *error = "heartbeat.timeout_ms must be greater than heartbeat.interval_ms";
    return false;
  }
  if (config.reconnect.initial_backoff_ms == 0 ||
      config.reconnect.max_backoff_ms == 0 ||
      config.reconnect.max_backoff_ms < config.reconnect.initial_backoff_ms) {
    *error = "invalid reconnect backoff window";
    return false;
  }
  if (config.reconnect.backoff_shift_bits > 31) {
    *error = "reconnect.backoff_shift_bits is out of range";
    return false;
  }
  if (config.reconnect.jitter_percent > 100) {
    *error = "reconnect.jitter_percent is out of range";
    return false;
  }
  return true;
}

}  // namespace

WebSocketConfigResult ParseWebSocketConfig(
    toml::node_view<const toml::node> node) {
  std::string error;
  if (!RequireTable(node, "data_session.websocket", &error)) {
    return ConfigFailure(std::move(error));
  }

  const toml::node_view<const toml::node> endpoint_node = node["endpoint"];
  if (!RequireTable(endpoint_node, "data_session.websocket.endpoint", &error)) {
    return ConfigFailure(std::move(error));
  }

  const toml::node_view<const toml::node> execution_policy_node =
      node["execution_policy"];
  if (!RequireTable(execution_policy_node,
                    "data_session.websocket.execution_policy", &error)) {
    return ConfigFailure(std::move(error));
  }

  WebSocketConfig config;
  if (!ParseEndpoint(endpoint_node, &config.endpoint, &error)) {
    return ConfigFailure(std::move(error));
  }
  if (!ParseExecutionPolicy(execution_policy_node, &config.execution_policy,
                            &error)) {
    return ConfigFailure(std::move(error));
  }

  const toml::node_view<const toml::node> read_path_node = node["read_path"];
  if (read_path_node) {
    if (!RequireTable(read_path_node, "data_session.websocket.read_path",
                      &error) ||
        !ParseReadPath(read_path_node, &config.read_path, &error)) {
      return ConfigFailure(std::move(error));
    }
  }

  const toml::node_view<const toml::node> heartbeat_node = node["heartbeat"];
  if (heartbeat_node) {
    if (!RequireTable(heartbeat_node, "data_session.websocket.heartbeat",
                      &error) ||
        !ParseHeartbeat(heartbeat_node, &config.heartbeat, &error)) {
      return ConfigFailure(std::move(error));
    }
  }

  const toml::node_view<const toml::node> reconnect_node = node["reconnect"];
  if (reconnect_node) {
    if (!RequireTable(reconnect_node, "data_session.websocket.reconnect",
                      &error) ||
        !ParseReconnect(reconnect_node, &config.reconnect, &error)) {
      return ConfigFailure(std::move(error));
    }
  }

  if (!ValidateWebSocketConfig(config, &error)) {
    return ConfigFailure(std::move(error));
  }
  return ConfigSuccess(std::move(config));
}

ConnectionConfigResult ToConnectionConfig(const WebSocketConfig& config,
                                          std::string target) {
  std::string error;
  if (target.empty()) {
    return ConnectionFailure("websocket target must be a non-empty string");
  }
  if (!ValidateWebSocketConfig(config, &error)) {
    return ConnectionFailure(std::move(error));
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
