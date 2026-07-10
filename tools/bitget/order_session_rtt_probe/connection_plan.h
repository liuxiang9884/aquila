#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_CONNECTION_PLAN_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_CONNECTION_PLAN_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aquila::tools::bitget_order_session_rtt_probe {

struct ProbeConnectionConfig {
  std::string name;
  std::string group;
  std::string host;
  std::string connect_ip;
  std::string port;
  bool enable_tls{true};
  std::int32_t worker_cpu_id{-1};
};

struct ProbeConnectionsCsvResult {
  bool ok{false};
  std::vector<ProbeConnectionConfig> connections;
  std::string error;
};

[[nodiscard]] ProbeConnectionsCsvResult LoadProbeConnectionsCsvFile(
    const std::filesystem::path& path);

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_CONNECTION_PLAN_H_
