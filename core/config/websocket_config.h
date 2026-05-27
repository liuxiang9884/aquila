#ifndef AQUILA_CORE_CONFIG_WEBSOCKET_CONFIG_H_
#define AQUILA_CORE_CONFIG_WEBSOCKET_CONFIG_H_

#include <cstdint>
#include <string>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/websocket/types.h"

namespace aquila::config {

struct WebSocketEndpointConfig {
  std::string host;
  std::string connect_ip;
  std::string port{"443"};
  bool enable_tls{true};
  std::uint32_t connect_timeout_ms{10'000};
};

struct WebSocketExecutionPolicyConfig {
  int bind_cpu_id{-1};
  websocket::AffinityMode affinity_mode{websocket::AffinityMode::kBestEffort};
  bool lock_memory{true};
  bool prefault_stack{true};
  bool active_spin{true};
  std::uint32_t spin_iterations_before_clock_check{4096};
};

struct WebSocketReadPathConfig {
  std::uint32_t max_reads_per_drive{8};
  bool read_until_would_block{false};
};

struct WebSocketHeartbeatConfig {
  std::uint32_t interval_ms{5000};
  std::uint32_t timeout_ms{15000};
};

struct WebSocketReconnectConfig {
  bool enabled{true};
  std::uint32_t initial_backoff_ms{100};
  std::uint32_t max_backoff_ms{30'000};
  std::uint32_t backoff_shift_bits{1};
  std::uint32_t jitter_percent{25};
  std::uint32_t max_attempts{0};
};

struct WebSocketConfig {
  WebSocketEndpointConfig endpoint;
  WebSocketExecutionPolicyConfig execution_policy;
  WebSocketReadPathConfig read_path;
  WebSocketHeartbeatConfig heartbeat;
  WebSocketReconnectConfig reconnect;
};

using WebSocketConfigResult = Result<WebSocketConfig>;
using ConnectionConfigResult = Result<websocket::ConnectionConfig>;

[[nodiscard]] WebSocketConfigResult ParseWebSocketConfig(
    toml::node_view<const toml::node> node);

[[nodiscard]] ConnectionConfigResult ToConnectionConfig(
    const WebSocketConfig& config, std::string target);

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_WEBSOCKET_CONFIG_H_
