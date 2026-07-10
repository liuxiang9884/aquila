#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_RUN_PLAN_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_RUN_PLAN_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/common/result.h"
#include "tools/bitget/order_session_rtt_probe/config.h"
#include "tools/bitget/order_session_rtt_probe/connection_plan.h"

namespace aquila::tools::bitget_order_session_rtt_probe {

struct ProbeCycle {
  std::uint32_t cycle_index{0};
  std::vector<ProbeConnectionConfig> connections;
};

struct ProbeRunPlan {
  std::size_t connection_count{0};
  std::vector<ProbeCycle> cycles;
};

using ProbeRunPlanResult = Result<ProbeRunPlan>;

[[nodiscard]] inline ProbeRunPlanResult BuildProbeRunPlan(
    const ProbeConfig& config, std::vector<ProbeConnectionConfig> connections) {
  ProbeRunPlanResult result;
  if (connections.empty()) {
    result.error = "probe run plan requires at least one connection";
    return result;
  }
  result.ok = true;
  result.value.connection_count = connections.size();
  result.value.cycles.reserve(config.sampling.samples_per_session);
  for (std::uint32_t i = 0; i < config.sampling.samples_per_session; ++i) {
    result.value.cycles.push_back(
        ProbeCycle{.cycle_index = i, .connections = connections});
  }
  return result;
}

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_RUN_PLAN_H_
