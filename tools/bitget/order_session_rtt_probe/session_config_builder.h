#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SESSION_CONFIG_BUILDER_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SESSION_CONFIG_BUILDER_H_

#include <utility>

#include "exchange/bitget/trading/order_session_config.h"
#include "tools/bitget/order_session_rtt_probe/connection_plan.h"

namespace aquila::tools::bitget_order_session_rtt_probe {

[[nodiscard]] inline bitget::OrderSessionConfig BuildPinnedOrderSessionConfig(
    bitget::OrderSessionConfig base, const ProbeConnectionConfig& connection) {
  base.connection.host = connection.host;
  base.connection.connect_ip = connection.connect_ip;
  base.connection.port = connection.port;
  base.connection.enable_tls = connection.enable_tls;
  if (connection.worker_cpu_id >= 0) {
    base.connection.runtime_policy.io_cpu_id = connection.worker_cpu_id;
  }
  return base;
}

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SESSION_CONFIG_BUILDER_H_
