#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_RUN_PLAN_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_RUN_PLAN_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common/result.h"
#include "tools/gate/order_session_rtt_probe/candidate_ip_list.h"
#include "tools/gate/order_session_rtt_probe/config.h"
#include "tools/gate/order_session_rtt_probe/cycle_scheduler.h"

namespace aquila::tools::gate_order_session_rtt_probe {

struct ProbeRunPlan {
  std::size_t candidate_ip_count{0};
  std::size_t duplicate_candidate_ip_count{0};
  std::vector<ProbeCycle> cycles;
};

using ProbeRunPlanResult = Result<ProbeRunPlan>;

[[nodiscard]] inline ProbeRunPlanResult BuildProbeRunPlanFromCandidateText(
    const ProbeConfig& config, std::string_view candidate_ip_text) {
  ProbeRunPlanResult result;
  CandidateIpLoadResult candidates = LoadCandidateIpsFromText(
      candidate_ip_text,
      CandidateIpLoadOptions{.max_candidates = config.sessions.max_candidates});
  if (!candidates.ok) {
    result.error = std::move(candidates.error);
    return result;
  }

  ProbeRunPlan plan;
  plan.candidate_ip_count = candidates.ips.size();
  plan.duplicate_candidate_ip_count = candidates.duplicate_count;

  CycleScheduler scheduler(CycleSchedulerOptions{
      .candidate_ips = std::move(candidates.ips),
      .active_session_count = config.sessions.active_session_count,
      .samples_per_ip = config.sampling.samples_per_ip,
      .cycles_per_connection_generation =
          config.sampling.cycles_per_connection_generation,
  });
  while (scheduler.HasNextCycle()) {
    plan.cycles.push_back(scheduler.NextCycle());
  }

  result.ok = true;
  result.value = std::move(plan);
  return result;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_RUN_PLAN_H_
