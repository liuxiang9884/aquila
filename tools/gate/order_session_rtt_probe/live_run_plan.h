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
#include "core/trading/order_id.h"
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
  std::uint64_t order_session_id{0};
  std::uint64_t local_order_id_first{1};
  std::uint64_t local_order_id_stride{4};
  LiveRunPaths paths;
  gate::OrderSessionConfig order_session_config;
};

struct MultiSessionLiveRunPlan {
  std::vector<SingleSessionLiveRunPlan> sessions;
  LiveRunPaths paths;
};

using SingleSessionLiveRunPlanResult = Result<SingleSessionLiveRunPlan>;
using MultiSessionLiveRunPlanResult = Result<MultiSessionLiveRunPlan>;

namespace live_run_plan_detail {

[[nodiscard]] inline SingleSessionLiveRunPlanResult Failure(std::string error) {
  SingleSessionLiveRunPlanResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] inline MultiSessionLiveRunPlanResult MultiFailure(
    std::string error) {
  MultiSessionLiveRunPlanResult result;
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

[[nodiscard]] inline std::optional<std::size_t> SessionIndexForLocalOrderId(
    std::uint64_t local_order_id, std::size_t session_count) noexcept {
  if (local_order_id == 0 || session_count == 0) {
    return std::nullopt;
  }
  const std::uint64_t strategy_order_id =
      LocalOrderIdCodec::StrategyOrderId(local_order_id);
  if (strategy_order_id == 0) {
    return std::nullopt;
  }
  const std::uint64_t sample_first_order_id =
      strategy_order_id - ((strategy_order_id - 1) % 4);
  const std::uint64_t raw_index = (sample_first_order_id - 1) / 4;
  return static_cast<std::size_t>(raw_index % session_count);
}

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
      .order_session_id = 0,
      .local_order_id_first = 1,
      .local_order_id_stride = 4,
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

[[nodiscard]] inline MultiSessionLiveRunPlanResult BuildMultiSessionLiveRunPlan(
    const ProbeConfig& config, const ProbeRunPlan& run_plan,
    const gate::OrderSessionConfig& base_order_session_config) {
  if (config.run_id.empty()) {
    return live_run_plan_detail::MultiFailure("run_id must be non-empty");
  }
  const std::size_t session_count = config.sessions.active_session_count;
  if (session_count <= 1) {
    return live_run_plan_detail::MultiFailure(
        "multi-session live run requires active_session_count > 1");
  }
  if (run_plan.cycles.empty()) {
    return live_run_plan_detail::MultiFailure(
        "multi-session live run has no cycles");
  }

  const std::vector<std::string>& first_connect_ips =
      run_plan.cycles.front().connect_ips;
  if (first_connect_ips.size() != session_count) {
    return live_run_plan_detail::MultiFailure(
        "multi-session live run requires every cycle to have "
        "active_session_count connect_ips");
  }
  for (const std::string& connect_ip : first_connect_ips) {
    if (connect_ip.empty()) {
      return live_run_plan_detail::MultiFailure(
          "multi-session live run requires non-empty connect_ip values");
    }
  }
  for (const ProbeCycle& cycle : run_plan.cycles) {
    if (cycle.connect_ips != first_connect_ips) {
      return live_run_plan_detail::MultiFailure(
          "multi-session live run requires every cycle to reuse the same "
          "connect_ip order");
    }
  }

  MultiSessionLiveRunPlanResult result;
  result.ok = true;
  result.value.paths = live_run_plan_detail::BuildLiveRunPaths(config);
  result.value.sessions.reserve(session_count);
  for (std::size_t i = 0; i < session_count; ++i) {
    std::optional<std::int32_t> worker_cpu_id;
    if (i < config.sessions.worker_cpu_ids.size()) {
      worker_cpu_id = config.sessions.worker_cpu_ids[i];
    }
    result.value.sessions.push_back(SingleSessionLiveRunPlan{
        .connect_ip = first_connect_ips[i],
        .sample_count = run_plan.cycles.size(),
        .order_session_id = static_cast<std::uint64_t>(i),
        .local_order_id_first = 1 + static_cast<std::uint64_t>(i) * 4,
        .local_order_id_stride = static_cast<std::uint64_t>(session_count) * 4,
        .paths = result.value.paths,
        .order_session_config = BuildPinnedOrderSessionConfig(
            base_order_session_config,
            PinnedOrderSessionOptions{
                .connect_ip = first_connect_ips[i],
                .worker_cpu_id = worker_cpu_id,
                .enable_tcp_info_diagnostics = config.sessions.enable_tcp_info,
            }),
    });
  }
  return result;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_LIVE_RUN_PLAN_H_
