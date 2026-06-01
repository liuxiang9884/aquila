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
  std::filesystem::path run_metadata_path;
  std::filesystem::path connection_observed_csv_path;
  std::filesystem::path rest_guard_csv_path;
  std::filesystem::path raw_rest_dir;
};

struct SingleSessionLiveRunPlan {
  std::string session_name;
  std::string group;
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
  paths.run_metadata_path =
      paths.run_dir / "order_session_rtt_run_metadata.json";
  paths.connection_observed_csv_path =
      paths.run_dir / "order_session_rtt_connections_observed.csv";
  paths.rest_guard_csv_path =
      paths.run_dir / "order_session_rtt_rest_guard.csv";
  paths.raw_rest_dir = paths.run_dir / "raw_rest";
  return paths;
}

[[nodiscard]] inline std::optional<std::int32_t> ConnectionWorkerCpuId(
    const ProbeConnectionConfig& connection) {
  if (connection.worker_cpu_id < 0) {
    return std::nullopt;
  }
  return connection.worker_cpu_id;
}

[[nodiscard]] inline PinnedOrderSessionOptions BuildPinnedOptions(
    const ProbeConnectionConfig& connection, bool enable_tcp_info,
    const websocket::SocketTimestampingConfig& timestamping) {
  return PinnedOrderSessionOptions{
      .connect_ip = connection.connect_ip,
      .host = std::optional<std::string>{connection.host},
      .port = std::optional<std::string>{connection.port},
      .enable_tls = connection.enable_tls,
      .worker_cpu_id = ConnectionWorkerCpuId(connection),
      .enable_tcp_info_diagnostics = enable_tcp_info,
      .timestamping = timestamping,
  };
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
  if (run_plan.cycles.empty()) {
    return live_run_plan_detail::Failure(
        "single-session live run has no cycles");
  }

  const std::vector<ProbeConnectionConfig>& first_connections =
      run_plan.cycles.front().connections;
  if (first_connections.size() != 1 || first_connections.front().name.empty()) {
    return live_run_plan_detail::Failure(
        "single-session live run requires exactly one connection per cycle");
  }
  const ProbeConnectionConfig connection = first_connections.front();
  for (const ProbeCycle& cycle : run_plan.cycles) {
    if (cycle.connections.size() != 1 ||
        cycle.connections.front().name != connection.name) {
      return live_run_plan_detail::Failure(
          "single-session live run requires all cycles to use the same "
          "connection");
    }
  }

  SingleSessionLiveRunPlanResult result;
  result.ok = true;
  result.value = SingleSessionLiveRunPlan{
      .session_name = connection.name,
      .group = connection.group,
      .connect_ip = connection.connect_ip,
      .sample_count = run_plan.cycles.size(),
      .order_session_id = 0,
      .local_order_id_first = 1,
      .local_order_id_stride = 4,
      .paths = live_run_plan_detail::BuildLiveRunPaths(config),
      .order_session_config = BuildPinnedOrderSessionConfig(
          base_order_session_config,
          live_run_plan_detail::BuildPinnedOptions(
              connection, config.sessions.enable_tcp_info,
              config.sessions.timestamping)),
  };
  return result;
}

[[nodiscard]] inline MultiSessionLiveRunPlanResult BuildMultiSessionLiveRunPlan(
    const ProbeConfig& config, const ProbeRunPlan& run_plan,
    const gate::OrderSessionConfig& base_order_session_config) {
  if (config.run_id.empty()) {
    return live_run_plan_detail::MultiFailure("run_id must be non-empty");
  }
  if (run_plan.cycles.empty()) {
    return live_run_plan_detail::MultiFailure(
        "multi-session live run has no cycles");
  }
  const std::vector<ProbeConnectionConfig>& first_connections =
      run_plan.cycles.front().connections;
  const std::size_t session_count = first_connections.size();
  if (session_count <= 1) {
    return live_run_plan_detail::MultiFailure(
        "multi-session live run requires more than one connection");
  }
  for (const ProbeConnectionConfig& connection : first_connections) {
    if (connection.name.empty() || connection.connect_ip.empty()) {
      return live_run_plan_detail::MultiFailure(
          "multi-session live run requires non-empty connection names and "
          "connect_ip values");
    }
  }
  for (const ProbeCycle& cycle : run_plan.cycles) {
    if (cycle.connections.size() != first_connections.size()) {
      return live_run_plan_detail::MultiFailure(
          "multi-session live run requires every cycle to reuse the same "
          "connection order");
    }
    for (std::size_t i = 0; i < first_connections.size(); ++i) {
      if (cycle.connections[i].name != first_connections[i].name) {
        return live_run_plan_detail::MultiFailure(
            "multi-session live run requires every cycle to reuse the same "
            "connection order");
      }
    }
  }

  MultiSessionLiveRunPlanResult result;
  result.ok = true;
  result.value.paths = live_run_plan_detail::BuildLiveRunPaths(config);
  result.value.sessions.reserve(session_count);
  for (std::size_t i = 0; i < session_count; ++i) {
    const ProbeConnectionConfig& connection = first_connections[i];
    result.value.sessions.push_back(SingleSessionLiveRunPlan{
        .session_name = connection.name,
        .group = connection.group,
        .connect_ip = connection.connect_ip,
        .sample_count = run_plan.cycles.size(),
        .order_session_id = static_cast<std::uint64_t>(i),
        .local_order_id_first = 1 + static_cast<std::uint64_t>(i) * 4,
        .local_order_id_stride = static_cast<std::uint64_t>(session_count) * 4,
        .paths = result.value.paths,
        .order_session_config = BuildPinnedOrderSessionConfig(
            base_order_session_config,
            live_run_plan_detail::BuildPinnedOptions(
                connection, config.sessions.enable_tcp_info,
                config.sessions.timestamping)),
    });
  }
  return result;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_LIVE_RUN_PLAN_H_
