#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SESSION_CONFIG_BUILDER_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SESSION_CONFIG_BUILDER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "exchange/gate/trading/order_session_config.h"

namespace aquila::tools::gate_order_session_rtt_probe {

struct PinnedOrderSessionOptions {
  std::string connect_ip;
  std::optional<std::string> host;
  std::optional<std::string> port;
  std::optional<bool> enable_tls;
  std::optional<std::int32_t> worker_cpu_id;
  bool enable_tcp_info_diagnostics{false};
};

[[nodiscard]] inline gate::OrderSessionConfig BuildPinnedOrderSessionConfig(
    gate::OrderSessionConfig base, PinnedOrderSessionOptions options) {
  if (options.host) {
    base.connection.host = std::move(*options.host);
  }
  base.connection.connect_ip = std::move(options.connect_ip);
  if (options.port) {
    base.connection.port = std::move(*options.port);
  }
  if (options.enable_tls) {
    base.connection.enable_tls = *options.enable_tls;
  }
  if (options.worker_cpu_id) {
    base.connection.runtime_policy.io_cpu_id = *options.worker_cpu_id;
  }
  base.enable_tcp_info_diagnostics = options.enable_tcp_info_diagnostics;
  return base;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SESSION_CONFIG_BUILDER_H_
