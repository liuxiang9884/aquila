#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include <CLI/CLI.hpp>
#include <fmt/core.h>

#include "exchange/gate/trading/order_session_config.h"
#include "tools/gate/order_session_rtt_probe/config.h"
#include "tools/gate/order_session_rtt_probe/live_run_plan.h"
#include "tools/gate/order_session_rtt_probe/run_plan.h"

namespace {

namespace probe = aquila::tools::gate_order_session_rtt_probe;

struct CliOptions {
  std::filesystem::path config_path{
      "config/order_session_rtt_probe/gate_order_session_rtt_probe.toml"};
  std::filesystem::path candidate_ip_file_override;
  std::optional<std::size_t> max_candidates_override;
  std::optional<std::uint32_t> samples_per_ip_override;
  std::optional<std::size_t> active_session_count_override;
  bool execute{false};
  bool live_preflight{false};
};

[[nodiscard]] aquila::Result<std::string> ReadTextFile(
    const std::filesystem::path& path) {
  aquila::Result<std::string> result;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    result.error = fmt::format("failed to open '{}'", path.string());
    return result;
  }
  result.value.assign(std::istreambuf_iterator<char>{file},
                      std::istreambuf_iterator<char>{});
  result.ok = true;
  return result;
}

struct OverrideResult {
  bool ok{false};
  std::string error;
};

[[nodiscard]] OverrideResult ApplyOverrides(const CliOptions& options,
                                            probe::ProbeConfig* config) {
  if (!options.candidate_ip_file_override.empty()) {
    config->inputs.candidate_ip_file = options.candidate_ip_file_override;
  }
  if (options.max_candidates_override) {
    config->sessions.max_candidates = *options.max_candidates_override;
  }
  if (options.samples_per_ip_override) {
    if (*options.samples_per_ip_override == 0) {
      return {.error = "--samples-per-ip must be positive"};
    }
    config->sampling.samples_per_ip = *options.samples_per_ip_override;
  }
  if (options.active_session_count_override) {
    if (*options.active_session_count_override == 0) {
      return {.error = "--active-session-count must be positive"};
    }
    config->sessions.active_session_count =
        *options.active_session_count_override;
  }
  if (options.execute) {
    config->execute = true;
  }
  return {.ok = true};
}

void EnsureRunId(probe::ProbeConfig* config) {
  if (!config->run_id.empty()) {
    return;
  }
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  config->run_id = fmt::format("gate_order_session_rtt_probe_{}", ns);
}

void PrintPlan(const probe::ProbeConfig& config,
               const probe::ProbeRunPlan& plan) {
  fmt::print(
      "gate_order_session_rtt_probe dry_run={} execute={} name={} run_id={} "
      "candidate_ip_file={} candidate_ips={} duplicate_candidate_ips={} "
      "active_session_count={} samples_per_ip={} cycles={} "
      "cycle_cooldown_ms={}\n",
      config.execute ? "false" : "true", config.execute ? "true" : "false",
      config.name, config.run_id, config.inputs.candidate_ip_file.string(),
      plan.candidate_ip_count, plan.duplicate_candidate_ip_count,
      config.sessions.active_session_count, config.sampling.samples_per_ip,
      plan.cycles.size(), config.sampling.cycle_cooldown_ms);

  for (const probe::ProbeCycle& cycle : plan.cycles) {
    fmt::print("cycle index={} group={} connect_ips=", cycle.cycle_index,
               cycle.group_index);
    for (std::size_t i = 0; i < cycle.connect_ips.size(); ++i) {
      if (i != 0) {
        fmt::print(",");
      }
      fmt::print("{}", cycle.connect_ips[i]);
    }
    fmt::print("\n");
  }
}

void PrintLivePreflightPlan(const probe::ProbeConfig& config,
                            const probe::SingleSessionLiveRunPlan& live_plan) {
  fmt::print(
      "gate_order_session_rtt_probe live_preflight=true execute=false "
      "name={} run_id={} connect_ip={} sample_count={} run_dir={} "
      "sample_csv_path={} rest_guard_csv_path={} raw_rest_dir={} "
      "order_session_host={} order_session_target={} "
      "order_session_worker_cpu={} enable_tcp_info={}\n",
      config.name, config.run_id, live_plan.connect_ip, live_plan.sample_count,
      live_plan.paths.run_dir.string(),
      live_plan.paths.sample_csv_path.string(),
      live_plan.paths.rest_guard_csv_path.string(),
      live_plan.paths.raw_rest_dir.string(),
      live_plan.order_session_config.connection.host,
      live_plan.order_session_config.connection.target,
      live_plan.order_session_config.connection.runtime_policy.io_cpu_id,
      live_plan.order_session_config.enable_tcp_info_diagnostics ? "true"
                                                                 : "false");
}

int Run(const CliOptions& options) {
  probe::ProbeConfigResult config_result =
      probe::LoadProbeConfigFile(options.config_path);
  if (!config_result.ok) {
    fmt::print(stderr, "[FAIL] config_error={}\n", config_result.error);
    return 1;
  }

  probe::ProbeConfig config = std::move(config_result.value);
  const OverrideResult override_result = ApplyOverrides(options, &config);
  if (!override_result.ok) {
    fmt::print(stderr, "[FAIL] option_error={}\n", override_result.error);
    return 2;
  }
  EnsureRunId(&config);
  if (config.execute) {
    fmt::print(stderr,
               "[FAIL] live execution is not implemented in this V1a "
               "dry-run scaffold\n");
    return 2;
  }

  aquila::Result<std::string> candidate_text =
      ReadTextFile(config.inputs.candidate_ip_file);
  if (!candidate_text.ok) {
    fmt::print(stderr, "[FAIL] candidate_ip_error={}\n", candidate_text.error);
    return 1;
  }

  probe::ProbeRunPlanResult plan_result =
      probe::BuildProbeRunPlanFromCandidateText(config, candidate_text.value);
  if (!plan_result.ok) {
    fmt::print(stderr, "[FAIL] run_plan_error={}\n", plan_result.error);
    return 1;
  }

  if (options.live_preflight) {
    aquila::gate::OrderSessionConfigResult order_session_config =
        aquila::gate::LoadOrderSessionConfigFile(
            config.inputs.order_session_config);
    if (!order_session_config.ok) {
      fmt::print(stderr, "[FAIL] order_session_config_error={}\n",
                 order_session_config.error);
      return 1;
    }
    probe::SingleSessionLiveRunPlanResult live_plan =
        probe::BuildSingleSessionLiveRunPlan(config, plan_result.value,
                                             order_session_config.value);
    if (!live_plan.ok) {
      fmt::print(stderr, "[FAIL] live_preflight_error={}\n", live_plan.error);
      return 1;
    }
    PrintLivePreflightPlan(config, live_plan.value);
    return 0;
  }

  PrintPlan(config, plan_result.value);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;

  CLI::App app{"Build a Gate order session RTT probe run plan"};
  app.add_option("--config", options.config_path, "Probe TOML path");
  app.add_option("--candidate-ip-file", options.candidate_ip_file_override,
                 "Override probe.inputs.candidate_ip_file");
  app.add_option("--max-candidates", options.max_candidates_override,
                 "Override probe.sessions.max_candidates");
  app.add_option("--samples-per-ip", options.samples_per_ip_override,
                 "Override probe.sampling.samples_per_ip");
  app.add_option("--active-session-count",
                 options.active_session_count_override,
                 "Override probe.sessions.active_session_count");
  app.add_flag("--execute", options.execute,
               "Enable live execution. Currently rejected by this scaffold");
  app.add_flag("--live-preflight", options.live_preflight,
               "Build single-session live prerequisites without connecting");
  CLI11_PARSE(app, argc, argv);

  try {
    return Run(options);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "[FAIL] fatal_error={}\n", exc.what());
    return 1;
  }
}
