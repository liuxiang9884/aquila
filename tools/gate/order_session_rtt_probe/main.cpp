#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
#include "tools/gate/order_session_rtt_probe/cycle_scheduler.h"
#include "tools/gate/order_session_rtt_probe/live_run_plan.h"
#include "tools/gate/order_session_rtt_probe/local_feedback_queue.h"
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

[[nodiscard]] std::string JoinConnectIps(
    const std::vector<std::string>& connect_ips) {
  std::string joined;
  for (std::size_t i = 0; i < connect_ips.size(); ++i) {
    if (i != 0) {
      joined.push_back(',');
    }
    joined.append(connect_ips[i]);
  }
  return joined;
}

void PrintPlan(const probe::ProbeConfig& config,
               const probe::ProbeRunPlan& plan) {
  NOVA_INFO(
      "gate_order_session_rtt_probe dry_run={} execute={} name={} run_id={} "
      "candidate_ip_file={} candidate_ips={} duplicate_candidate_ips={} "
      "active_session_count={} samples_per_ip={} cycles={} "
      "cycle_cooldown_ms={} order_session_interval_ms={} order_mode={}",
      config.execute ? "false" : "true", config.execute ? "true" : "false",
      config.name, config.run_id, config.inputs.candidate_ip_file.string(),
      plan.candidate_ip_count, plan.duplicate_candidate_ip_count,
      config.sessions.active_session_count, config.sampling.samples_per_ip,
      plan.cycles.size(), config.sampling.cycle_cooldown_ms,
      config.sampling.order_session_interval_ms,
      magic_enum::enum_name(config.order.order_mode));

  for (const probe::ProbeCycle& cycle : plan.cycles) {
    NOVA_INFO("cycle index={} group={} connect_ips={}", cycle.cycle_index,
              cycle.group_index, JoinConnectIps(cycle.connect_ips));
  }
}

[[nodiscard]] std::filesystem::path SessionSampleCsvPath(
    const probe::MultiSessionLiveRunPlan& plan, std::size_t session_index) {
  return plan.paths.run_dir /
         fmt::format("order_session_rtt_samples_session_{}.csv", session_index);
}

[[nodiscard]] std::int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] bool MergeSampleCsvFiles(
    const std::vector<std::filesystem::path>& input_paths,
    const std::filesystem::path& output_path, std::string* error) {
  try {
    const std::filesystem::path parent = output_path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      if (error != nullptr) {
        *error =
            fmt::format("failed to open merged CSV '{}'", output_path.string());
      }
      return false;
    }

    bool wrote_header = false;
    for (const std::filesystem::path& input_path : input_paths) {
      std::ifstream input(input_path, std::ios::binary);
      if (!input) {
        if (error != nullptr) {
          *error = fmt::format("failed to open session CSV '{}'",
                               input_path.string());
        }
        return false;
      }
      std::string line;
      bool first_line = true;
      while (std::getline(input, line)) {
        if (first_line) {
          first_line = false;
          if (wrote_header) {
            continue;
          }
          wrote_header = true;
        }
        output << line << '\n';
      }
    }
  } catch (const std::exception& exc) {
    if (error != nullptr) {
      *error = fmt::format("failed to merge session CSVs: {}", exc.what());
    }
    return false;
  }
  return true;
}

template <typename WebSocketPolicy>
int RunSingleSessionExecute(probe::ProbeConfig config,
                            const probe::SingleSessionLiveRunPlan& live_plan,
                            aquila::gate::LoginCredentials credentials,
                            double duration_sec) {
  if (config.order.order_mode != probe::ProbeOrderMode::kIoc) {
    NOVA_ERROR(
        "execute currently supports probe.order.order_mode=ioc for the first "
        "single-session smoke");
    return 2;
  }

  aquila::config::DataReaderConfigResult data_reader_config =
      aquila::config::LoadDataReaderConfigFile(
          config.inputs.data_reader_config);
  if (!data_reader_config.ok) {
    NOVA_ERROR("data_reader_config_error={}", data_reader_config.error);
    return 1;
  }

  aquila::config::OrderFeedbackShmConfigResult feedback_shm_config =
      aquila::config::LoadOrderFeedbackShmConfigFile(
          config.feedback.shm_config);
  if (!feedback_shm_config.ok) {
    NOVA_ERROR("feedback_shm_config_error={}", feedback_shm_config.error);
    return 1;
  }

  auto feedback_manager = aquila::OrderFeedbackShmManager::Open(
      ToFeedbackShmConfig(feedback_shm_config.value));
  if (!feedback_manager.ok) {
    NOVA_ERROR("feedback_shm_open_error={}", feedback_manager.error);
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
    NOVA_ERROR("feedback_reader_error={}", feedback_reader.error);
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
    NOVA_ERROR("sample_csv_error={}", csv_error);
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
              live_plan.order_session_config.enable_tcp_info_diagnostics,
          .ack_latency =
              live_plan.order_session_config.ack_latency_diagnostics});
  Runner runner(config, live_plan, instrument_catalog, data_reader,
                feedback_reader.value, sample_writer, duration_sec);
  runner.BindSession(session);
  handler.context = &runner;
  handler.on_login_ready = &ProbeLoginReadyCallback<Runner>;
  handler.on_login_not_ready = &ProbeLoginNotReadyCallback<Runner>;
  handler.on_order_response = &ProbeOrderResponseCallback<Runner>;
  session.SetRuntimeHook(&runner, &Runner::RuntimeHookCallback);

  NOVA_INFO(
      "gate_order_session_rtt_probe execute=true live_single_session=true "
      "run_id={} connect_ip={} order_session_host={} order_session_port={} "
      "order_session_tls={} samples={} duration_sec={:.3f} "
      "cycle_cooldown_ms={} order_session_interval_ms={} order_mode={} "
      "sample_csv_path={}",
      config.run_id, live_plan.connect_ip,
      live_plan.order_session_config.connection.host,
      live_plan.order_session_config.connection.port,
      live_plan.order_session_config.connection.enable_tls ? "true" : "false",
      live_plan.sample_count, duration_sec, config.sampling.cycle_cooldown_ms,
      config.sampling.order_session_interval_ms,
      magic_enum::enum_name(config.order.order_mode),
      live_plan.paths.sample_csv_path.string());

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
  NOVA_INFO(
      "gate_order_session_rtt_probe_summary start_result={} exit_code={} "
      "stop_reason={} samples_started={} samples_completed={} "
      "samples_failed={} data_reader_events={} feedback_events={} "
      "skipped_book_tickers={} runner_stop_observed={} "
      "duration_watchdog_fired={} sample_csv_path={}",
      watchdog_result.start_result ? "true" : "false", exit_code, stop_reason,
      stats.samples_started, stats.samples_completed, stats.samples_failed,
      stats.data_reader_events, stats.feedback_events,
      stats.skipped_book_tickers,
      watchdog_result.runner_stop_observed ? "true" : "false",
      watchdog_result.duration_watchdog_fired ? "true" : "false",
      live_plan.paths.sample_csv_path.string());
  return exit_code;
}

using ProbeDataReader = aquila::market_data::RealtimeDataReader<
    aquila::market_data::RealtimeDataReaderDiagnostics>;

class LiveSessionStateBase {
 public:
  virtual ~LiveSessionStateBase() = default;

  [[nodiscard]] virtual const probe::SingleSessionLiveRunPlan& plan()
      const noexcept = 0;
  [[nodiscard]] virtual bool returned() const noexcept = 0;
  [[nodiscard]] virtual bool stopping() const noexcept = 0;
  [[nodiscard]] virtual bool start_result() const noexcept = 0;
  [[nodiscard]] virtual int exit_code() const noexcept = 0;
  [[nodiscard]] virtual std::string_view stop_reason() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t samples_started() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t data_reader_events() const noexcept = 0;
  [[nodiscard]] virtual const probe::SingleSessionLiveRunnerStats& stats()
      const noexcept = 0;

  virtual bool OpenSampleWriter(std::string* error) = 0;
  virtual void EnableExternalDispatchGate() noexcept = 0;
  virtual void StartThread() = 0;
  virtual void Stop() noexcept = 0;
  virtual void JoinAndClose() = 0;
  virtual void PushFeedback(const aquila::OrderFeedbackEvent& event) = 0;
  virtual void GrantDispatch() noexcept = 0;
};

template <typename WebSocketPolicy>
class LiveSessionState final : public LiveSessionStateBase {
 public:
  using Session =
      aquila::gate::OrderSession<ProbeRawResponseHandler, WebSocketPolicy,
                                 aquila::gate::OrderSessionDiagnostics>;
  using Runner = probe::SingleSessionLiveRunner<Session, ProbeDataReader,
                                                probe::LocalFeedbackQueue>;

  LiveSessionState(const probe::ProbeConfig& config_ref,
                   probe::SingleSessionLiveRunPlan plan_in,
                   const aquila::gate::LoginCredentials& credentials_ref,
                   aquila::config::DataReaderConfig data_reader_config,
                   double duration_sec)
      : plan_(std::move(plan_in)),
        instrument_catalog_(data_reader_config.instrument_catalog),
        data_reader_(std::move(data_reader_config)),
        session_(plan_.order_session_config.connection,
                 aquila::gate::LoginCredentials(credentials_ref), handler_,
                 plan_.order_session_config.request_map_capacity,
                 aquila::gate::OrderSessionSocketDiagnosticsConfig{
                     .enable_tcp_info =
                         plan_.order_session_config.enable_tcp_info_diagnostics,
                     .ack_latency =
                         plan_.order_session_config.ack_latency_diagnostics}),
        runner_(config_ref, plan_, instrument_catalog_, data_reader_,
                feedback_queue_, sample_writer_, duration_sec) {
    runner_.BindSession(session_);
    handler_.context = &runner_;
    handler_.on_login_ready = &ProbeLoginReadyCallback<Runner>;
    handler_.on_login_not_ready = &ProbeLoginNotReadyCallback<Runner>;
    handler_.on_order_response = &ProbeOrderResponseCallback<Runner>;
    session_.SetRuntimeHook(&runner_, &Runner::RuntimeHookCallback);
  }

  [[nodiscard]] const probe::SingleSessionLiveRunPlan& plan()
      const noexcept override {
    return plan_;
  }

  [[nodiscard]] bool returned() const noexcept override {
    return returned_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool stopping() const noexcept override {
    return runner_.stopping();
  }

  [[nodiscard]] bool start_result() const noexcept override {
    return start_result_;
  }

  [[nodiscard]] int exit_code() const noexcept override {
    return runner_.exit_code();
  }

  [[nodiscard]] std::string_view stop_reason() const noexcept override {
    return runner_.stop_reason();
  }

  [[nodiscard]] std::uint64_t samples_started() const noexcept override {
    return runner_.samples_started();
  }

  [[nodiscard]] std::uint64_t data_reader_events() const noexcept override {
    return runner_.data_reader_events();
  }

  [[nodiscard]] const probe::SingleSessionLiveRunnerStats& stats()
      const noexcept override {
    return runner_.stats();
  }

  bool OpenSampleWriter(std::string* error) override {
    return sample_writer_.Open(plan_.paths.sample_csv_path, error);
  }

  void EnableExternalDispatchGate() noexcept override {
    runner_.EnableExternalDispatchGate();
  }

  void StartThread() override {
    thread_ = std::thread([this] {
      start_result_ = session_.Start();
      returned_.store(true, std::memory_order_release);
    });
  }

  void Stop() noexcept override {
    session_.Stop();
  }

  void JoinAndClose() override {
    if (!returned()) {
      session_.Stop();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    sample_writer_.Close();
  }

  void PushFeedback(const aquila::OrderFeedbackEvent& event) override {
    feedback_queue_.Push(event);
  }

  void GrantDispatch() noexcept override {
    runner_.GrantDispatch();
  }

 private:
  probe::SingleSessionLiveRunPlan plan_;
  aquila::config::InstrumentCatalog instrument_catalog_;
  ProbeDataReader data_reader_;
  probe::LocalFeedbackQueue feedback_queue_;
  probe::SampleCsvWriter sample_writer_;
  ProbeRawResponseHandler handler_;
  Session session_;
  Runner runner_;
  std::atomic<bool> returned_{false};
  bool start_result_{false};
  std::thread thread_;
};

[[nodiscard]] std::unique_ptr<LiveSessionStateBase> MakeLiveSessionState(
    const probe::ProbeConfig& config,
    probe::SingleSessionLiveRunPlan session_plan,
    const aquila::gate::LoginCredentials& credentials,
    aquila::config::DataReaderConfig data_reader_config, double duration_sec) {
  if (session_plan.order_session_config.connection.enable_tls) {
    return std::make_unique<
        LiveSessionState<aquila::gate::OrderSessionDefaultTlsWebSocketPolicy>>(
        config, std::move(session_plan), credentials,
        std::move(data_reader_config), duration_sec);
  }
  return std::make_unique<
      LiveSessionState<aquila::gate::OrderSessionDefaultPlainWebSocketPolicy>>(
      config, std::move(session_plan), credentials,
      std::move(data_reader_config), duration_sec);
}

int RunMultiSessionExecute(probe::ProbeConfig config,
                           const probe::MultiSessionLiveRunPlan& live_plan,
                           const aquila::gate::LoginCredentials& credentials,
                           double duration_sec) {
  if (config.order.order_mode != probe::ProbeOrderMode::kIoc) {
    NOVA_ERROR(
        "execute currently supports probe.order.order_mode=ioc for "
        "multi-session live smoke");
    return 2;
  }

  aquila::config::OrderFeedbackShmConfigResult feedback_shm_config =
      aquila::config::LoadOrderFeedbackShmConfigFile(
          config.feedback.shm_config);
  if (!feedback_shm_config.ok) {
    NOVA_ERROR("feedback_shm_config_error={}", feedback_shm_config.error);
    return 1;
  }

  auto feedback_manager = aquila::OrderFeedbackShmManager::Open(
      ToFeedbackShmConfig(feedback_shm_config.value));
  if (!feedback_manager.ok) {
    NOVA_ERROR("feedback_shm_open_error={}", feedback_manager.error);
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
    NOVA_ERROR("feedback_reader_error={}", feedback_reader.error);
    return 1;
  }

  std::vector<std::filesystem::path> session_csv_paths;
  session_csv_paths.reserve(live_plan.sessions.size());
  std::vector<std::unique_ptr<LiveSessionStateBase>> sessions;
  sessions.reserve(live_plan.sessions.size());

  for (std::size_t i = 0; i < live_plan.sessions.size(); ++i) {
    aquila::config::DataReaderConfigResult data_reader_config =
        aquila::config::LoadDataReaderConfigFile(
            config.inputs.data_reader_config);
    if (!data_reader_config.ok) {
      NOVA_ERROR("data_reader_config_error={}", data_reader_config.error);
      return 1;
    }

    probe::SingleSessionLiveRunPlan session_plan = live_plan.sessions[i];
    session_plan.paths.sample_csv_path = SessionSampleCsvPath(live_plan, i);
    session_csv_paths.push_back(session_plan.paths.sample_csv_path);

    std::unique_ptr<LiveSessionStateBase> state =
        MakeLiveSessionState(config, std::move(session_plan), credentials,
                             std::move(data_reader_config.value), duration_sec);
    state->EnableExternalDispatchGate();
    std::string csv_error;
    if (!state->OpenSampleWriter(&csv_error)) {
      NOVA_ERROR("sample_csv_error={}", csv_error);
      return 1;
    }
    sessions.push_back(std::move(state));
  }

  NOVA_INFO(
      "gate_order_session_rtt_probe execute=true live_multi_session=true "
      "run_id={} session_count={} samples_per_session={} total_samples={} "
      "duration_sec={:.3f} cycle_cooldown_ms={} "
      "order_session_interval_ms={} order_mode={} sample_csv_path={}",
      config.run_id, sessions.size(),
      sessions.empty() ? 0 : sessions.front()->plan().sample_count,
      sessions.empty()
          ? 0
          : sessions.size() * sessions.front()->plan().sample_count,
      duration_sec, config.sampling.cycle_cooldown_ms,
      config.sampling.order_session_interval_ms,
      magic_enum::enum_name(config.order.order_mode),
      live_plan.paths.sample_csv_path.string());
  for (const std::unique_ptr<LiveSessionStateBase>& state : sessions) {
    NOVA_INFO(
        "gate_order_session_rtt_probe_session index={} connect_ip={} "
        "samples={} order_session_host={} order_session_port={} "
        "order_session_tls={} order_session_worker_cpu={} "
        "session_sample_csv_path={}",
        state->plan().order_session_id, state->plan().connect_ip,
        state->plan().sample_count,
        state->plan().order_session_config.connection.host,
        state->plan().order_session_config.connection.port,
        state->plan().order_session_config.connection.enable_tls ? "true"
                                                                 : "false",
        state->plan().order_session_config.connection.runtime_policy.io_cpu_id,
        state->plan().paths.sample_csv_path.string());
  }

  for (std::unique_ptr<LiveSessionStateBase>& state : sessions) {
    state->StartThread();
  }

  auto all_returned = [&sessions] {
    for (const std::unique_ptr<LiveSessionStateBase>& state : sessions) {
      if (!state->returned()) {
        return false;
      }
    }
    return true;
  };
  auto all_runners_stopping = [&sessions] {
    for (const std::unique_ptr<LiveSessionStateBase>& state : sessions) {
      if (!state->stopping()) {
        return false;
      }
    }
    return true;
  };
  auto stop_running_sessions = [&sessions] {
    for (std::unique_ptr<LiveSessionStateBase>& state : sessions) {
      if (!state->returned()) {
        state->Stop();
      }
    }
  };

  const auto start = std::chrono::steady_clock::now();
  const bool has_deadline = duration_sec > 0.0;
  const auto deadline =
      start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(duration_sec) +
                  std::chrono::duration<double>(30.0));
  bool runner_stop_observed = false;
  bool duration_watchdog_fired = false;
  std::uint64_t feedback_events_consumed = 0;
  std::uint64_t feedback_events_routed = 0;
  std::uint64_t feedback_events_broadcast = 0;
  std::uint64_t feedback_events_unrouted = 0;
  probe::MultiSessionDispatchScheduler dispatch_scheduler(
      probe::MultiSessionDispatchSchedulerOptions{
          .session_count = sessions.size(),
          .sample_count_per_session =
              sessions.empty() ? 0 : sessions.front()->plan().sample_count,
          .order_session_interval_ms =
              config.sampling.order_session_interval_ms,
          .cycle_cooldown_ms = config.sampling.cycle_cooldown_ms,
      });
  std::vector<std::uint64_t> samples_started_by_session(sessions.size(), 0);

  while (!all_returned()) {
    const std::size_t consumed = feedback_reader.value.Poll(
        config.feedback.poll_budget,
        [&](const aquila::OrderFeedbackEvent& event) {
          if (event.kind == aquila::OrderFeedbackKind::kContinuityLost ||
              event.local_order_id == 0) {
            for (std::unique_ptr<LiveSessionStateBase>& state : sessions) {
              state->PushFeedback(event);
            }
            ++feedback_events_broadcast;
            return;
          }
          std::optional<std::size_t> session_index =
              probe::SessionIndexForLocalOrderId(event.local_order_id,
                                                 sessions.size());
          if (!session_index || *session_index >= sessions.size()) {
            ++feedback_events_unrouted;
            return;
          }
          sessions[*session_index]->PushFeedback(event);
          ++feedback_events_routed;
        });
    feedback_events_consumed += consumed;

    std::uint64_t total_market_events = 0;
    for (std::size_t i = 0; i < sessions.size(); ++i) {
      samples_started_by_session[i] = sessions[i]->samples_started();
      total_market_events += sessions[i]->data_reader_events();
    }
    std::size_t dispatch_session_index = 0;
    if (dispatch_scheduler.NextGrant(total_market_events,
                                     samples_started_by_session, SteadyNowNs(),
                                     &dispatch_session_index)) {
      sessions[dispatch_session_index]->GrantDispatch();
    }

    if (!runner_stop_observed && all_runners_stopping()) {
      runner_stop_observed = true;
      stop_running_sessions();
    }
    if (has_deadline && std::chrono::steady_clock::now() >= deadline) {
      duration_watchdog_fired = true;
      stop_running_sessions();
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  for (std::unique_ptr<LiveSessionStateBase>& state : sessions) {
    state->JoinAndClose();
  }

  std::string merge_error;
  const bool merge_ok = MergeSampleCsvFiles(
      session_csv_paths, live_plan.paths.sample_csv_path, &merge_error);
  if (!merge_ok) {
    NOVA_ERROR("sample_csv_merge_error={}", merge_error);
  }

  int exit_code = merge_ok ? 0 : 1;
  for (const std::unique_ptr<LiveSessionStateBase>& state : sessions) {
    const probe::SingleSessionLiveRunnerStats& stats = state->stats();
    int session_exit_code = state->start_result() ? state->exit_code() : 1;
    if (duration_watchdog_fired && !state->stopping()) {
      session_exit_code = 2;
    }
    if (session_exit_code != 0) {
      exit_code = session_exit_code;
    }
    NOVA_INFO(
        "gate_order_session_rtt_probe_session_summary index={} "
        "connect_ip={} start_result={} exit_code={} stop_reason={} "
        "samples_started={} samples_completed={} samples_failed={} "
        "data_reader_events={} feedback_events={} skipped_book_tickers={} "
        "sample_csv_path={}",
        state->plan().order_session_id, state->plan().connect_ip,
        state->start_result() ? "true" : "false", session_exit_code,
        state->stop_reason(), stats.samples_started, stats.samples_completed,
        stats.samples_failed, stats.data_reader_events, stats.feedback_events,
        stats.skipped_book_tickers,
        state->plan().paths.sample_csv_path.string());
  }
  NOVA_INFO(
      "gate_order_session_rtt_probe_summary start_result={} exit_code={} "
      "stop_reason={} runner_stop_observed={} duration_watchdog_fired={} "
      "feedback_events_consumed={} feedback_events_routed={} "
      "feedback_events_broadcast={} feedback_events_unrouted={} "
      "sample_csv_path={}",
      exit_code == 0 ? "true" : "false", exit_code,
      duration_watchdog_fired
          ? "duration_watchdog_fired"
          : (merge_ok ? "multi_session_complete" : "sample_csv_merge_failed"),
      runner_stop_observed ? "true" : "false",
      duration_watchdog_fired ? "true" : "false", feedback_events_consumed,
      feedback_events_routed, feedback_events_broadcast,
      feedback_events_unrouted, live_plan.paths.sample_csv_path.string());
  return exit_code;
}

void PrintLivePreflightPlan(const probe::ProbeConfig& config,
                            const probe::SingleSessionLiveRunPlan& live_plan) {
  NOVA_INFO(
      "gate_order_session_rtt_probe live_preflight=true execute=false "
      "name={} run_id={} connect_ip={} sample_count={} run_dir={} "
      "sample_csv_path={} rest_guard_csv_path={} raw_rest_dir={} "
      "order_session_host={} order_session_port={} order_session_target={} "
      "order_session_tls={} "
      "order_session_worker_cpu={} enable_tcp_info={} "
      "order_session_interval_ms={} order_mode={}",
      config.name, config.run_id, live_plan.connect_ip, live_plan.sample_count,
      live_plan.paths.run_dir.string(),
      live_plan.paths.sample_csv_path.string(),
      live_plan.paths.rest_guard_csv_path.string(),
      live_plan.paths.raw_rest_dir.string(),
      live_plan.order_session_config.connection.host,
      live_plan.order_session_config.connection.port,
      live_plan.order_session_config.connection.target,
      live_plan.order_session_config.connection.enable_tls ? "true" : "false",
      live_plan.order_session_config.connection.runtime_policy.io_cpu_id,
      live_plan.order_session_config.enable_tcp_info_diagnostics ? "true"
                                                                 : "false",
      config.sampling.order_session_interval_ms,
      magic_enum::enum_name(config.order.order_mode));
}

void PrintMultiLivePreflightPlan(
    const probe::ProbeConfig& config,
    const probe::MultiSessionLiveRunPlan& live_plan) {
  const std::size_t sample_count =
      live_plan.sessions.empty() ? 0 : live_plan.sessions.front().sample_count;
  NOVA_INFO(
      "gate_order_session_rtt_probe live_preflight=true execute=false "
      "live_multi_session=true name={} run_id={} session_count={} "
      "sample_count_per_session={} total_sample_count={} run_dir={} "
      "sample_csv_path={} rest_guard_csv_path={} raw_rest_dir={} "
      "order_session_interval_ms={} order_mode={}",
      config.name, config.run_id, live_plan.sessions.size(), sample_count,
      sample_count * live_plan.sessions.size(),
      live_plan.paths.run_dir.string(),
      live_plan.paths.sample_csv_path.string(),
      live_plan.paths.rest_guard_csv_path.string(),
      live_plan.paths.raw_rest_dir.string(),
      config.sampling.order_session_interval_ms,
      magic_enum::enum_name(config.order.order_mode));
  for (const probe::SingleSessionLiveRunPlan& session : live_plan.sessions) {
    NOVA_INFO(
        "gate_order_session_rtt_probe_session index={} connect_ip={} "
        "sample_count={} order_session_host={} order_session_port={} "
        "order_session_target={} order_session_tls={} "
        "order_session_worker_cpu={} local_order_id_first={} "
        "local_order_id_stride={} enable_tcp_info={}",
        session.order_session_id, session.connect_ip, session.sample_count,
        session.order_session_config.connection.host,
        session.order_session_config.connection.port,
        session.order_session_config.connection.target,
        session.order_session_config.connection.enable_tls ? "true" : "false",
        session.order_session_config.connection.runtime_policy.io_cpu_id,
        session.local_order_id_first, session.local_order_id_stride,
        session.order_session_config.enable_tcp_info_diagnostics ? "true"
                                                                 : "false");
  }
}

int Run(const CliOptions& options, const toml::table& toml) {
  probe::ProbeConfigResult config_result = probe::ParseProbeConfig(toml);
  if (!config_result.ok) {
    NOVA_ERROR("config_error={}", config_result.error);
    return 1;
  }

  probe::ProbeConfig config = std::move(config_result.value);
  const OverrideResult override_result = ApplyOverrides(options, &config);
  if (!override_result.ok) {
    NOVA_ERROR("option_error={}", override_result.error);
    return 2;
  }
  EnsureRunId(&config);

  aquila::Result<std::string> candidate_text =
      ReadTextFile(config.inputs.candidate_ip_file);
  if (!candidate_text.ok) {
    NOVA_ERROR("candidate_ip_error={}", candidate_text.error);
    return 1;
  }

  probe::ProbeRunPlanResult plan_result =
      probe::BuildProbeRunPlanFromCandidateText(config, candidate_text.value);
  if (!plan_result.ok) {
    NOVA_ERROR("run_plan_error={}", plan_result.error);
    return 1;
  }

  if (options.live_preflight || config.execute) {
    aquila::gate::OrderSessionConfigResult order_session_config =
        aquila::gate::LoadOrderSessionConfigFile(
            config.inputs.order_session_config);
    if (!order_session_config.ok) {
      NOVA_ERROR("order_session_config_error={}", order_session_config.error);
      return 1;
    }
    if (config.execute) {
      const char* api_key =
          EnvValue(order_session_config.value.credentials.api_key_env);
      if (api_key == nullptr) {
        NOVA_ERROR("missing env var {}",
                   order_session_config.value.credentials.api_key_env);
        return 2;
      }
      const char* api_secret =
          EnvValue(order_session_config.value.credentials.api_secret_env);
      if (api_secret == nullptr) {
        NOVA_ERROR("missing env var {}",
                   order_session_config.value.credentials.api_secret_env);
        return 2;
      }
      aquila::gate::LoginCredentials credentials{.api_key = api_key,
                                                 .api_secret = api_secret};
      if (config.sessions.active_session_count == 1) {
        probe::SingleSessionLiveRunPlanResult live_plan =
            probe::BuildSingleSessionLiveRunPlan(config, plan_result.value,
                                                 order_session_config.value);
        if (!live_plan.ok) {
          NOVA_ERROR("live_preflight_error={}", live_plan.error);
          return 1;
        }
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
      probe::MultiSessionLiveRunPlanResult live_plan =
          probe::BuildMultiSessionLiveRunPlan(config, plan_result.value,
                                              order_session_config.value);
      if (!live_plan.ok) {
        NOVA_ERROR("live_preflight_error={}", live_plan.error);
        return 1;
      }
      return RunMultiSessionExecute(std::move(config), live_plan.value,
                                    credentials, options.duration_sec);
    }
    if (config.sessions.active_session_count == 1) {
      probe::SingleSessionLiveRunPlanResult live_plan =
          probe::BuildSingleSessionLiveRunPlan(config, plan_result.value,
                                               order_session_config.value);
      if (!live_plan.ok) {
        NOVA_ERROR("live_preflight_error={}", live_plan.error);
        return 1;
      }
      PrintLivePreflightPlan(config, live_plan.value);
      return 0;
    }
    probe::MultiSessionLiveRunPlanResult live_plan =
        probe::BuildMultiSessionLiveRunPlan(config, plan_result.value,
                                            order_session_config.value);
    if (!live_plan.ok) {
      NOVA_ERROR("live_preflight_error={}", live_plan.error);
      return 1;
    }
    PrintMultiLivePreflightPlan(config, live_plan.value);
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
  app.add_flag("--execute", options.execute, "Enable live execution");
  app.add_flag("--live-preflight", options.live_preflight,
               "Build live prerequisites without connecting");
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
    try {
      if (nova::kLogManager.logger() == nullptr) {
        nova::LogConfig fallback_log_config;
        fallback_log_config.set_file_sink_name(
            "/home/liuxiang/tmp/gate_order_session_rtt_probe_config_error.log");
        nova::InitializeLogging(fallback_log_config);
      }
      NOVA_ERROR("config_error={}", exc.what());
    } catch (...) {
    }
    return 1;
  }
}
