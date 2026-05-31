#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_RUN_PLAN_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_RUN_PLAN_H_

#include <cstddef>
#include <utility>
#include <vector>

#include "core/common/result.h"
#include "tools/gate/order_session_rtt_probe/config.h"
#include "tools/gate/order_session_rtt_probe/connection_plan.h"
#include "tools/gate/order_session_rtt_probe/cycle_scheduler.h"

namespace aquila::tools::gate_order_session_rtt_probe {

struct ProbeRunPlan {
  std::size_t connection_count{0};
  std::vector<ProbeCycle> cycles;
};

using ProbeRunPlanResult = Result<ProbeRunPlan>;

[[nodiscard]] inline ProbeRunPlanResult BuildProbeRunPlan(
    const ProbeConfig& config, std::vector<ProbeConnectionConfig> connections) {
  ProbeRunPlan plan;
  plan.connection_count = connections.size();

  CycleScheduler scheduler(CycleSchedulerOptions{
      .connections = std::move(connections),
      .samples_per_session = config.sampling.samples_per_session,
      .cycles_per_connection_generation =
          config.sampling.cycles_per_connection_generation,
  });
  while (scheduler.HasNextCycle()) {
    plan.cycles.push_back(scheduler.NextCycle());
  }

  ProbeRunPlanResult result;
  result.ok = true;
  result.value = std::move(plan);
  return result;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_RUN_PLAN_H_
