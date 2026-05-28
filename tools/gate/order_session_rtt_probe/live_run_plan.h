#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_LIVE_RUN_PLAN_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_LIVE_RUN_PLAN_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/common/result.h"
#include "exchange/gate/trading/order_session_config.h"
#include "tools/gate/order_session_rtt_probe/config.h"
#include "tools/gate/order_session_rtt_probe/run_plan.h"
#include "tools/gate/order_session_rtt_probe/session_config_builder.h"

namespace aquila::tools::gate_order_session_rtt_probe {

struct LiveRunPaths {
  std::filesystem::path run_dir;
  std::filesystem::path sample_csv_path;
  std::filesystem::path rest_guard_csv_path;
  std::filesystem::path raw_rest_dir;
};

struct SingleSessionLiveRunPlan {
  std::string connect_ip;
  std::size_t sample_count{0};
  LiveRunPaths paths;
  gate::OrderSessionConfig order_session_config;
};

using SingleSessionLiveRunPlanResult = Result<SingleSessionLiveRunPlan>;

namespace live_run_plan_detail {

[[nodiscard]] inline SingleSessionLiveRunPlanResult Failure(std::string error) {
  SingleSessionLiveRunPlanResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] inline LiveRunPaths BuildLiveRunPaths(const ProbeConfig& config) {
  LiveRunPaths paths;
  paths.run_dir = config.output.root_dir / config.run_id;
  paths.sample_csv_path = paths.run_dir / "order_session_rtt_samples.csv";
  paths.rest_guard_csv_path =
      paths.run_dir / "order_session_rtt_rest_guard.csv";
  paths.raw_rest_dir = paths.run_dir / "raw_rest";
  return paths;
}

}  // namespace live_run_plan_detail

[[nodiscard]] inline SingleSessionLiveRunPlanResult
BuildSingleSessionLiveRunPlan(
    const ProbeConfig& config, const ProbeRunPlan& run_plan,
    const gate::OrderSessionConfig& base_order_session_config) {
  if (config.run_id.empty()) {
    return live_run_plan_detail::Failure("run_id must be non-empty");
  }
  if (config.sessions.active_session_count != 1) {
    return live_run_plan_detail::Failure(
        "single-session live run requires active_session_count=1");
  }
  if (run_plan.cycles.empty()) {
    return live_run_plan_detail::Failure(
        "single-session live run has no cycles");
  }

  const std::vector<std::string>& first_connect_ips =
      run_plan.cycles.front().connect_ips;
  if (first_connect_ips.size() != 1 || first_connect_ips.front().empty()) {
    return live_run_plan_detail::Failure(
        "single-session live run requires exactly one connect_ip per cycle");
  }
  const std::string connect_ip = first_connect_ips.front();
  for (const ProbeCycle& cycle : run_plan.cycles) {
    if (cycle.connect_ips.size() != 1 ||
        cycle.connect_ips.front() != connect_ip) {
      return live_run_plan_detail::Failure(
          "single-session live run requires all cycles to use the same "
          "connect_ip");
    }
  }

  std::optional<std::int32_t> worker_cpu_id;
  if (!config.sessions.worker_cpu_ids.empty()) {
    worker_cpu_id = config.sessions.worker_cpu_ids.front();
  }

  SingleSessionLiveRunPlanResult result;
  result.ok = true;
  result.value = SingleSessionLiveRunPlan{
      .connect_ip = connect_ip,
      .sample_count = run_plan.cycles.size(),
      .paths = live_run_plan_detail::BuildLiveRunPaths(config),
      .order_session_config = BuildPinnedOrderSessionConfig(
          base_order_session_config,
          PinnedOrderSessionOptions{
              .connect_ip = connect_ip,
              .worker_cpu_id = worker_cpu_id,
              .enable_tcp_info_diagnostics = config.sessions.enable_tcp_info,
          }),
  };
  return result;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_LIVE_RUN_PLAN_H_
