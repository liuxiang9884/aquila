#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/config/order_feedback_shm_config.h"
#include "core/market_data/realtime_data_reader.h"
#include "core/trading/order_feedback_shm.h"
#include "exchange/gate/trading/order_session.h"
#include "exchange/gate/trading/order_session_config.h"
#include "nova/utils/log.h"
#include "tools/gate/order_session_rtt_probe/config.h"
#include "tools/gate/order_session_rtt_probe/live_run_plan.h"
#include "tools/gate/order_session_rtt_probe/run_plan.h"
#include "tools/gate/order_session_rtt_probe/session_watchdog.h"
#include "tools/gate/order_session_rtt_probe/single_session_live_runner.h"

namespace {

namespace probe = aquila::tools::gate_order_session_rtt_probe;

struct CliOptions {
  std::filesystem::path config_path{
      "config/order_session_rtt_probe/gate_order_session_rtt_probe.toml"};
  std::filesystem::path candidate_ip_file_override;
  std::optional<std::size_t> max_candidates_override;
  std::optional<std::uint32_t> samples_per_ip_override;
  std::optional<std::size_t> active_session_count_override;
  double duration_sec{0.0};
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

const char* EnvValue(const std::string& name) {
  if (name.empty()) {
    return nullptr;
  }
  const char* value = std::getenv(name.c_str());
  if (value == nullptr || value[0] == '\0') {
    return nullptr;
  }
  return value;
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
  if (options.duration_sec < 0.0) {
    return {.error = "--duration-sec must be non-negative"};
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

aquila::OrderFeedbackShmConfig ToFeedbackShmConfig(
    const aquila::config::OrderFeedbackShmRuntimeConfig& config) {
  return aquila::OrderFeedbackShmConfig{
      .shm_name = config.shm_name,
      .channel_name = config.channel_name,
      .create = false,
      .remove_existing = false,
  };
}

struct ProbeRawResponseHandler {
  void* context{nullptr};
  void (*on_login_ready)(void*) noexcept {nullptr};
  void (*on_login_not_ready)(void*) noexcept {nullptr};
  void (*on_order_response)(void*,
                            const aquila::gate::OrderResponse&) noexcept {
      nullptr};

  void OnOrderSessionLoginReady() noexcept {
    if (on_login_ready != nullptr) {
      on_login_ready(context);
    }
  }

  void OnOrderSessionLoginNotReady() noexcept {
    if (on_login_not_ready != nullptr) {
      on_login_not_ready(context);
    }
  }

  void OnOrderResponse(const aquila::gate::OrderResponse& response) noexcept {
    if (on_order_response != nullptr) {
      on_order_response(context, response);
    }
  }
};

template <typename Runner>
void ProbeLoginReadyCallback(void* raw) noexcept {
  static_cast<Runner*>(raw)->OnLoginReady();
}

template <typename Runner>
void ProbeLoginNotReadyCallback(void* raw) noexcept {
  static_cast<Runner*>(raw)->OnLoginNotReady();
}

template <typename Runner>
void ProbeOrderResponseCallback(
    void* raw, const aquila::gate::OrderResponse& response) noexcept {
  static_cast<Runner*>(raw)->OnOrderResponse(response);
}

void PrintPlan(const probe::ProbeConfig& config,
               const probe::ProbeRunPlan& plan) {
  fmt::print(
      "gate_order_session_rtt_probe dry_run={} execute={} name={} run_id={} "
      "candidate_ip_file={} candidate_ips={} duplicate_candidate_ips={} "
      "active_session_count={} samples_per_ip={} cycles={} "
      "cycle_cooldown_ms={} order_session_interval_ms={} order_mode={}\n",
      config.execute ? "false" : "true", config.execute ? "true" : "false",
      config.name, config.run_id, config.inputs.candidate_ip_file.string(),
      plan.candidate_ip_count, plan.duplicate_candidate_ip_count,
      config.sessions.active_session_count, config.sampling.samples_per_ip,
      plan.cycles.size(), config.sampling.cycle_cooldown_ms,
      config.sampling.order_session_interval_ms,
      magic_enum::enum_name(config.order.order_mode));

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

template <typename WebSocketPolicy>
int RunSingleSessionExecute(probe::ProbeConfig config,
                            const probe::SingleSessionLiveRunPlan& live_plan,
                            aquila::gate::LoginCredentials credentials,
                            double duration_sec) {
  if (config.order.order_mode != probe::ProbeOrderMode::kIoc) {
    fmt::print(stderr,
               "[FAIL] execute currently supports probe.order.order_mode=ioc "
               "for the first single-session smoke\n");
    return 2;
  }

  aquila::config::DataReaderConfigResult data_reader_config =
      aquila::config::LoadDataReaderConfigFile(
          config.inputs.data_reader_config);
  if (!data_reader_config.ok) {
    fmt::print(stderr, "[FAIL] data_reader_config_error={}\n",
               data_reader_config.error);
    return 1;
  }

  aquila::config::OrderFeedbackShmConfigResult feedback_shm_config =
      aquila::config::LoadOrderFeedbackShmConfigFile(
          config.feedback.shm_config);
  if (!feedback_shm_config.ok) {
    fmt::print(stderr, "[FAIL] feedback_shm_config_error={}\n",
               feedback_shm_config.error);
    return 1;
  }

  auto feedback_manager = aquila::OrderFeedbackShmManager::Open(
      ToFeedbackShmConfig(feedback_shm_config.value));
  if (!feedback_manager.ok) {
    fmt::print(stderr, "[FAIL] feedback_shm_open_error={}\n",
               feedback_manager.error);
    return 1;
  }

  const std::uint64_t raw_consumer_run_id = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto feedback_reader = aquila::OrderFeedbackShmReader::Claim(
      feedback_manager.value.channel(),
      static_cast<std::uint8_t>(config.feedback.strategy_id),
      raw_consumer_run_id == 0 ? 1 : raw_consumer_run_id,
      config.feedback.force_claim);
  if (!feedback_reader.ok) {
    fmt::print(stderr, "[FAIL] feedback_reader_error={}\n",
               feedback_reader.error);
    return 1;
  }

  using DataReader = aquila::market_data::RealtimeDataReader<
      aquila::market_data::RealtimeDataReaderDiagnostics>;
  aquila::config::InstrumentCatalog instrument_catalog =
      data_reader_config.value.instrument_catalog;
  DataReader data_reader(std::move(data_reader_config.value));

  probe::SampleCsvWriter sample_writer;
  std::string csv_error;
  if (!sample_writer.Open(live_plan.paths.sample_csv_path, &csv_error)) {
    fmt::print(stderr, "[FAIL] sample_csv_error={}\n", csv_error);
    return 1;
  }

  ProbeRawResponseHandler handler;
  using Session =
      aquila::gate::OrderSession<ProbeRawResponseHandler, WebSocketPolicy,
                                 aquila::gate::OrderSessionDiagnostics>;
  using Runner = probe::SingleSessionLiveRunner<Session, DataReader>;

  Session session(
      live_plan.order_session_config.connection, std::move(credentials),
      handler, live_plan.order_session_config.request_map_capacity,
      aquila::gate::OrderSessionSocketDiagnosticsConfig{
          .enable_tcp_info =
              live_plan.order_session_config.enable_tcp_info_diagnostics});
  Runner runner(config, live_plan, instrument_catalog, data_reader,
                feedback_reader.value, sample_writer, duration_sec);
  runner.BindSession(session);
  handler.context = &runner;
  handler.on_login_ready = &ProbeLoginReadyCallback<Runner>;
  handler.on_login_not_ready = &ProbeLoginNotReadyCallback<Runner>;
  handler.on_order_response = &ProbeOrderResponseCallback<Runner>;
  session.SetRuntimeHook(&runner, &Runner::RuntimeHookCallback);

  fmt::print(
      "gate_order_session_rtt_probe execute=true live_single_session=true "
      "run_id={} connect_ip={} samples={} duration_sec={:.3f} "
      "cycle_cooldown_ms={} order_session_interval_ms={} order_mode={} "
      "sample_csv_path={}\n",
      config.run_id, live_plan.connect_ip, live_plan.sample_count, duration_sec,
      config.sampling.cycle_cooldown_ms,
      config.sampling.order_session_interval_ms,
      magic_enum::enum_name(config.order.order_mode),
      live_plan.paths.sample_csv_path.string());

  std::fflush(stdout);
  const probe::SessionWatchdogResult watchdog_result =
      probe::RunSessionWithWatchdog(
          session, [&runner] { return runner.stopping(); }, duration_sec,
          /*watchdog_grace_sec=*/30.0);
  sample_writer.Close();
  const probe::SingleSessionLiveRunnerStats& stats = runner.stats();
  int exit_code = watchdog_result.start_result ? runner.exit_code() : 1;
  std::string stop_reason{runner.stop_reason()};
  if (watchdog_result.duration_watchdog_fired && !runner.stopping()) {
    exit_code = 2;
    stop_reason = "duration_watchdog_fired";
  }
  fmt::print(
      "gate_order_session_rtt_probe_summary start_result={} exit_code={} "
      "stop_reason={} samples_started={} samples_completed={} "
      "samples_failed={} data_reader_events={} feedback_events={} "
      "skipped_book_tickers={} runner_stop_observed={} "
      "duration_watchdog_fired={} sample_csv_path={}\n",
      watchdog_result.start_result ? "true" : "false", exit_code, stop_reason,
      stats.samples_started, stats.samples_completed, stats.samples_failed,
      stats.data_reader_events, stats.feedback_events,
      stats.skipped_book_tickers,
      watchdog_result.runner_stop_observed ? "true" : "false",
      watchdog_result.duration_watchdog_fired ? "true" : "false",
      live_plan.paths.sample_csv_path.string());
  return exit_code;
}

void PrintLivePreflightPlan(const probe::ProbeConfig& config,
                            const probe::SingleSessionLiveRunPlan& live_plan) {
  fmt::print(
      "gate_order_session_rtt_probe live_preflight=true execute=false "
      "name={} run_id={} connect_ip={} sample_count={} run_dir={} "
      "sample_csv_path={} rest_guard_csv_path={} raw_rest_dir={} "
      "order_session_host={} order_session_target={} "
      "order_session_worker_cpu={} enable_tcp_info={} "
      "order_session_interval_ms={} order_mode={}\n",
      config.name, config.run_id, live_plan.connect_ip, live_plan.sample_count,
      live_plan.paths.run_dir.string(),
      live_plan.paths.sample_csv_path.string(),
      live_plan.paths.rest_guard_csv_path.string(),
      live_plan.paths.raw_rest_dir.string(),
      live_plan.order_session_config.connection.host,
      live_plan.order_session_config.connection.target,
      live_plan.order_session_config.connection.runtime_policy.io_cpu_id,
      live_plan.order_session_config.enable_tcp_info_diagnostics ? "true"
                                                                 : "false",
      config.sampling.order_session_interval_ms,
      magic_enum::enum_name(config.order.order_mode));
}

int Run(const CliOptions& options, const toml::table& toml) {
  probe::ProbeConfigResult config_result = probe::ParseProbeConfig(toml);
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

  if (options.live_preflight || config.execute) {
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
    if (config.execute) {
      const char* api_key =
          EnvValue(order_session_config.value.credentials.api_key_env);
      if (api_key == nullptr) {
        fmt::print(stderr, "[FAIL] missing env var {}\n",
                   order_session_config.value.credentials.api_key_env);
        return 2;
      }
      const char* api_secret =
          EnvValue(order_session_config.value.credentials.api_secret_env);
      if (api_secret == nullptr) {
        fmt::print(stderr, "[FAIL] missing env var {}\n",
                   order_session_config.value.credentials.api_secret_env);
        return 2;
      }
      aquila::gate::LoginCredentials credentials{.api_key = api_key,
                                                 .api_secret = api_secret};
      if (live_plan.value.order_session_config.connection.enable_tls) {
        return RunSingleSessionExecute<
            aquila::gate::OrderSessionDefaultTlsWebSocketPolicy>(
            std::move(config), live_plan.value, std::move(credentials),
            options.duration_sec);
      }
      return RunSingleSessionExecute<
          aquila::gate::OrderSessionDefaultPlainWebSocketPolicy>(
          std::move(config), live_plan.value, std::move(credentials),
          options.duration_sec);
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
  app.add_option("--duration-sec", options.duration_sec,
                 "Maximum live execute duration. 0 means sample-count bounded");
  app.add_flag("--execute", options.execute,
               "Enable single-session live execution");
  app.add_flag("--live-preflight", options.live_preflight,
               "Build single-session live prerequisites without connecting");
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::parse_result toml =
        toml::parse_file(options.config_path.string());
    nova::LoggingGuard logging_guard{toml};
    NOVA_INFO(
        "gate_order_session_rtt_probe_start config_path={} execute={} "
        "live_preflight={} duration_sec={}",
        options.config_path.string(), options.execute ? "true" : "false",
        options.live_preflight ? "true" : "false", options.duration_sec);
    return Run(options, toml);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "[FAIL] config_error={}\n", exc.what());
    return 1;
  }
}
