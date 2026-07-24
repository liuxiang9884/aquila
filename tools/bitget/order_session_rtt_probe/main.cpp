#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <arpa/inet.h>
#include <fmt/core.h>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/config/order_feedback_shm_config.h"
#include "core/market_data/realtime_data_reader.h"
#include "core/trading/order_feedback_shm.h"
#include "exchange/bitget/trading/order_session.h"
#include "exchange/bitget/trading/order_session_config.h"
#include "nova/utils/log.h"
#include "tools/bitget/order_session_rtt_probe/config.h"
#include "tools/bitget/order_session_rtt_probe/connection_plan.h"
#include "tools/bitget/order_session_rtt_probe/execute_support.h"
#include "tools/bitget/order_session_rtt_probe/live_runner.h"
#include "tools/bitget/order_session_rtt_probe/local_feedback_queue.h"
#include "tools/bitget/order_session_rtt_probe/run_plan.h"
#include "tools/bitget/order_session_rtt_probe/sample_csv_writer.h"
#include "tools/bitget/order_session_rtt_probe/sequential_coordinator.h"
#include "tools/bitget/order_session_rtt_probe/session_config_builder.h"
#include <sched.h>
#include <signal.h>

namespace {

namespace probe = aquila::tools::bitget_order_session_rtt_probe;
namespace bitget = aquila::bitget;

struct CliOptions {
  std::filesystem::path config_path{
      "config/order_session_rtt_probe/bitget_order_session_rtt_probe.toml"};
  std::filesystem::path connections_file_override;
  std::optional<std::uint32_t> samples_per_session_override;
  double duration_sec{30.0};
  bool live_preflight{false};
  bool execute{false};
  bool confirm_dedicated_account{false};
};

volatile std::sig_atomic_t g_stop_requested = 0;

void HandleSignal(int) noexcept {
  g_stop_requested = 1;
}

[[nodiscard]] std::int64_t SteadyNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] std::int64_t RealtimeNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void EnsureRunId(probe::ProbeConfig* config) {
  if (!config->run_id.empty()) {
    return;
  }
  config->run_id =
      fmt::format("bitget_order_session_rtt_probe_{}", RealtimeNowNs());
}

struct OverrideResult {
  bool ok{false};
  std::string error;
};

[[nodiscard]] OverrideResult ApplyOverrides(const CliOptions& options,
                                            probe::ProbeConfig* config) {
  if (!options.connections_file_override.empty()) {
    config->inputs.connections_file = options.connections_file_override;
  }
  if (options.samples_per_session_override.has_value()) {
    if (*options.samples_per_session_override == 0) {
      return {.error = "--samples-per-session must be positive"};
    }
    config->sampling.samples_per_session =
        *options.samples_per_session_override;
  }
  return {.ok = true};
}

[[nodiscard]] bool ReadCredential(std::string_view env_name,
                                  std::string* output) {
  if (env_name.empty()) {
    return false;
  }
  const char* value = std::getenv(std::string(env_name).c_str());
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  output->assign(value);
  return true;
}

[[nodiscard]] bool LoadCredentials(const bitget::OrderSessionConfig& config,
                                   bitget::LoginCredentials* credentials,
                                   std::string* error) {
  if (!ReadCredential(config.credentials.api_key_env, &credentials->api_key)) {
    *error = fmt::format("missing credential environment variable '{}'",
                         config.credentials.api_key_env);
    return false;
  }
  if (!ReadCredential(config.credentials.api_secret_env,
                      &credentials->api_secret)) {
    *error = fmt::format("missing credential environment variable '{}'",
                         config.credentials.api_secret_env);
    return false;
  }
  if (!ReadCredential(config.credentials.api_passphrase_env,
                      &credentials->passphrase)) {
    *error = fmt::format("missing credential environment variable '{}'",
                         config.credentials.api_passphrase_env);
    return false;
  }
  return true;
}

[[nodiscard]] aquila::OrderFeedbackShmConfig ToOpenOnlyShmConfig(
    const aquila::config::OrderFeedbackShmRuntimeConfig& config) {
  return aquila::OrderFeedbackShmConfig{
      .shm_name = config.shm_name,
      .channel_name = config.channel_name,
      .create = false,
      .remove_existing = false,
  };
}

[[nodiscard]] std::string JoinConnections(
    const std::vector<probe::ProbeConnectionConfig>& connections) {
  std::string joined;
  for (std::size_t i = 0; i < connections.size(); ++i) {
    if (i != 0) {
      joined.push_back(',');
    }
    joined.append(connections[i].name);
    joined.push_back('=');
    joined.append(
        connections[i].connect_ip.empty() ? "dns" : connections[i].connect_ip);
  }
  return joined;
}

void PrintPlan(const probe::ProbeConfig& config,
               const probe::ProbeRunPlan& plan, bool live_preflight) {
  NOVA_INFO(
      "bitget_order_session_rtt_probe dry_run={} live_preflight={} "
      "execute=false name={} run_id={} symbol={} connections_file={} "
      "connections={} samples_per_session={} cycles={} cycle_cooldown_us={} "
      "order_session_interval_us={} orders_sent=0 "
      "rest_guard_implemented=false",
      live_preflight ? "false" : "true", live_preflight ? "true" : "false",
      config.name, config.run_id, config.order.symbol,
      config.inputs.connections_file.string(), plan.connection_count,
      config.sampling.samples_per_session, plan.cycles.size(),
      config.sampling.cycle_cooldown_us,
      config.sampling.order_session_interval_us);
  if (!plan.cycles.empty()) {
    NOVA_INFO("bitget_order_session_rtt_probe_connections {}",
              JoinConnections(plan.cycles.front().connections));
  }
}

struct PreflightResult {
  bool ok{false};
  bitget::OrderSessionConfig order_session_config;
  std::string error;
};

[[nodiscard]] PreflightResult ValidateLivePreflight(
    const probe::ProbeConfig& config) {
  bitget::OrderSessionConfigResult order_session =
      bitget::LoadOrderSessionConfigFile(config.inputs.order_session_config);
  if (!order_session.ok) {
    return {.error = order_session.error};
  }
  if (!order_session.value.client_oid_run_namespace.IsConfigured()) {
    return {.error =
                "Bitget RTT probe requires a run-scoped "
                "client_oid_run_namespace"};
  }
  if (order_session.value.category != "usdt-futures" ||
      order_session.value.position_mode != "one_way_mode" ||
      order_session.value.margin_mode != "crossed") {
    return {.error =
                "Bitget RTT probe requires usdt-futures, one_way_mode and "
                "crossed margin"};
  }

  aquila::config::DataReaderConfigResult data_reader =
      aquila::config::LoadDataReaderConfigFile(
          config.inputs.data_reader_config);
  if (!data_reader.ok) {
    return {.error = data_reader.error};
  }
  if (data_reader.value.instrument_catalog.Find(
          aquila::Exchange::kBitget, config.order.symbol) == nullptr) {
    return {.error = fmt::format(
                "Bitget instrument '{}' is absent from data reader catalog",
                config.order.symbol)};
  }
  bool has_bitget_book_ticker = false;
  for (const aquila::config::DataReaderSourceConfig& source :
       data_reader.value.sources) {
    has_bitget_book_ticker =
        has_bitget_book_ticker ||
        (source.exchange == aquila::Exchange::kBitget &&
         source.feed == aquila::config::DataReaderFeed::kBookTicker);
  }
  if (!has_bitget_book_ticker) {
    return {.error =
                "data reader requires at least one Bitget book_ticker source"};
  }

  aquila::config::OrderFeedbackShmConfigResult feedback =
      aquila::config::LoadOrderFeedbackShmConfigFile(
          config.feedback.shm_config);
  if (!feedback.ok) {
    return {.error = feedback.error};
  }
  return {.ok = true, .order_session_config = std::move(order_session.value)};
}

[[nodiscard]] bool BindCoordinatorCpu(int cpu_id, std::string* error) {
  if (cpu_id < 0) {
    return true;
  }
  if (cpu_id >= CPU_SETSIZE) {
    *error = "probe.sampling.coordinator_cpu exceeds CPU_SETSIZE";
    return false;
  }
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(cpu_id, &cpu_set);
  const int rc =
      ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set), &cpu_set);
  if (rc != 0) {
    *error =
        fmt::format("failed to bind coordinator CPU {} error={}", cpu_id, rc);
    return false;
  }
  return true;
}

[[nodiscard]] bool NumericIpEqual(std::string_view lhs, std::string_view rhs) {
  in_addr lhs_v4{};
  in_addr rhs_v4{};
  const std::string lhs_text(lhs);
  const std::string rhs_text(rhs);
  if (::inet_pton(AF_INET, lhs_text.c_str(), &lhs_v4) == 1 &&
      ::inet_pton(AF_INET, rhs_text.c_str(), &rhs_v4) == 1) {
    return lhs_v4.s_addr == rhs_v4.s_addr;
  }
  in6_addr lhs_v6{};
  in6_addr rhs_v6{};
  return ::inet_pton(AF_INET6, lhs_text.c_str(), &lhs_v6) == 1 &&
         ::inet_pton(AF_INET6, rhs_text.c_str(), &rhs_v6) == 1 &&
         std::equal(std::begin(lhs_v6.s6_addr), std::end(lhs_v6.s6_addr),
                    std::begin(rhs_v6.s6_addr));
}

struct ProbeResponseHandler {
  void* context{nullptr};
  void (*on_login_ready)(void*) noexcept {nullptr};
  void (*on_login_not_ready)(void*) noexcept {nullptr};
  void (*on_response)(void*, const bitget::OrderResponse&) noexcept {nullptr};
  void (*on_connected)(void*,
                       const bitget::OrderSessionConnectionInfo&) noexcept {
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
  void OnOrderResponse(const bitget::OrderResponse& response) noexcept {
    if (on_response != nullptr) {
      on_response(context, response);
    }
  }
  void OnOrderSessionConnected(
      const bitget::OrderSessionConnectionInfo& info) noexcept {
    if (on_connected != nullptr) {
      on_connected(context, info);
    }
  }
};

template <typename RunnerT>
void LoginReadyCallback(void* context) noexcept {
  static_cast<RunnerT*>(context)->OnLoginReady();
}

template <typename RunnerT>
void LoginNotReadyCallback(void* context) noexcept {
  static_cast<RunnerT*>(context)->OnLoginNotReady();
}

template <typename RunnerT>
void ResponseCallback(void* context,
                      const bitget::OrderResponse& response) noexcept {
  static_cast<RunnerT*>(context)->OnOrderResponse(response);
}

template <typename RunnerT>
void ConnectedCallback(
    void* context, const bitget::OrderSessionConnectionInfo& info) noexcept {
  static_cast<RunnerT*>(context)->OnOrderSessionConnected(info);
}

using ProbeDataReader = aquila::market_data::RealtimeDataReader<
    aquila::market_data::RealtimeDataReaderDiagnostics>;

class LiveSessionStateBase {
 public:
  virtual ~LiveSessionStateBase() = default;

  [[nodiscard]] virtual const probe::ProbeConnectionConfig& connection()
      const noexcept = 0;
  [[nodiscard]] virtual bool StartThread(std::string* error) = 0;
  virtual void StopGracefully() noexcept = 0;
  virtual void RequestAbort() noexcept = 0;
  virtual void Join() noexcept = 0;
  virtual void GrantDispatch() noexcept = 0;
  [[nodiscard]] virtual bool returned() const noexcept = 0;
  [[nodiscard]] virtual bool start_result() const noexcept = 0;
  [[nodiscard]] virtual bool login_ready() const noexcept = 0;
  [[nodiscard]] virtual bool failed() const noexcept = 0;
  [[nodiscard]] virtual bool has_active_sample() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t samples_finished() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t samples_completed() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t samples_failed() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t orders_sent() const noexcept = 0;
  [[nodiscard]] virtual std::string_view failure_reason() const noexcept = 0;
  [[nodiscard]] virtual probe::LocalFeedbackQueue*
  feedback_queue() noexcept = 0;
  [[nodiscard]] virtual bool WriteConnectionObservation(
      probe::ConnectionObservedCsvWriter* writer, std::string* error) const = 0;
};

template <typename WebSocketPolicy>
class LiveSessionState final : public LiveSessionStateBase {
 public:
  using Session = bitget::OrderSession<ProbeResponseHandler, WebSocketPolicy,
                                       bitget::OrderSessionDiagnostics>;
  using Runner =
      probe::LiveRunner<Session, ProbeDataReader, probe::LocalFeedbackQueue,
                        probe::SampleCsvWriter>;

  LiveSessionState(const probe::ProbeConfig& config,
                   probe::ProbeConnectionConfig connection,
                   std::size_t session_index, std::size_t session_count,
                   bitget::OrderSessionConfig order_session_config,
                   const bitget::LoginCredentials& credentials,
                   aquila::config::DataReaderConfig data_reader_config,
                   probe::SampleCsvWriter& sample_writer)
      : config_(config),
        connection_(std::move(connection)),
        order_session_config_(probe::BuildPinnedOrderSessionConfig(
            std::move(order_session_config), connection_)),
        instrument_catalog_(data_reader_config.instrument_catalog),
        data_reader_(std::move(data_reader_config)),
        feedback_queue_(config.sampling.feedback_queue_capacity),
        session_(order_session_config_.connection,
                 bitget::LoginCredentials(credentials),
                 order_session_config_.client_oid_run_namespace, handler_,
                 order_session_config_.request_map_capacity,
                 order_session_config_.order_id_cache_capacity),
        runner_(config_, connection_, session_index, session_count,
                instrument_catalog_, data_reader_, feedback_queue_,
                sample_writer) {
    runner_.BindSession(session_);
    handler_.context = &runner_;
    handler_.on_login_ready = &LoginReadyCallback<Runner>;
    handler_.on_login_not_ready = &LoginNotReadyCallback<Runner>;
    handler_.on_response = &ResponseCallback<Runner>;
    handler_.on_connected = &ConnectedCallback<Runner>;
    session_.SetRuntimeHook(&runner_, &Runner::RuntimeHookCallback);
  }

  ~LiveSessionState() override {
    StopGracefully();
    Join();
  }

  [[nodiscard]] const probe::ProbeConnectionConfig& connection()
      const noexcept override {
    return connection_;
  }

  [[nodiscard]] bool StartThread(std::string* error) override {
    try {
      thread_ = std::thread([this] {
        start_result_ = session_.Start();
        returned_.store(true, std::memory_order_release);
      });
      return true;
    } catch (const std::exception& ex) {
      *error = fmt::format("failed to start session thread '{}': {}",
                           connection_.name, ex.what());
      return false;
    }
  }

  void StopGracefully() noexcept override {
    runner_.PrepareGracefulStop();
    session_.Stop();
  }

  void RequestAbort() noexcept override {
    runner_.RequestAbort();
  }

  void Join() noexcept override {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void GrantDispatch() noexcept override {
    runner_.GrantDispatch();
  }

  [[nodiscard]] bool returned() const noexcept override {
    return returned_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool start_result() const noexcept override {
    return start_result_;
  }
  [[nodiscard]] bool login_ready() const noexcept override {
    return runner_.login_ready();
  }
  [[nodiscard]] bool failed() const noexcept override {
    return runner_.failed();
  }
  [[nodiscard]] bool has_active_sample() const noexcept override {
    return runner_.has_active_sample();
  }
  [[nodiscard]] std::uint64_t samples_finished() const noexcept override {
    return runner_.samples_finished();
  }
  [[nodiscard]] std::uint64_t samples_completed() const noexcept override {
    return runner_.samples_completed();
  }
  [[nodiscard]] std::uint64_t samples_failed() const noexcept override {
    return runner_.samples_failed();
  }
  [[nodiscard]] std::uint64_t orders_sent() const noexcept override {
    return runner_.orders_sent();
  }
  [[nodiscard]] std::string_view failure_reason() const noexcept override {
    return runner_.failure_reason();
  }
  [[nodiscard]] probe::LocalFeedbackQueue* feedback_queue() noexcept override {
    return &feedback_queue_;
  }

  [[nodiscard]] bool WriteConnectionObservation(
      probe::ConnectionObservedCsvWriter* writer,
      std::string* error) const override {
    if (!runner_.connection_observed()) {
      *error = fmt::format("session '{}' has no connection observation",
                           connection_.name);
      return false;
    }
    const bitget::OrderSessionConnectionInfo& info = runner_.connection_info();
    if (!writer->Write(
            probe::ConnectionObservedCsvRow{
                .run_id = config_.run_id,
                .session_name = connection_.name,
                .group = connection_.group,
                .configured_host = connection_.host,
                .configured_connect_ip = connection_.connect_ip,
                .configured_port = connection_.port,
                .worker_cpu = connection_.worker_cpu_id,
                .connected_at_ns = runner_.connection_observed_at_ns(),
                .endpoint_available = info.endpoint_available,
                .local_ip = info.local_ip,
                .local_port = info.local_port,
                .remote_ip = info.remote_ip,
                .remote_port = info.remote_port,
                .owner_thread_cpu = info.owner_thread_cpu,
                .owner_thread_tid = info.owner_thread_tid,
            },
            error)) {
      return false;
    }
    if (!info.endpoint_available) {
      *error = fmt::format("session '{}' endpoint observation unavailable",
                           connection_.name);
      return false;
    }
    if (!connection_.connect_ip.empty() &&
        !NumericIpEqual(connection_.connect_ip, info.remote_ip)) {
      *error = fmt::format(
          "session '{}' remote IP '{}' does not match configured '{}'",
          connection_.name, info.remote_ip, connection_.connect_ip);
      return false;
    }
    if (connection_.worker_cpu_id >= 0 &&
        info.owner_thread_cpu != connection_.worker_cpu_id) {
      *error = fmt::format(
          "session '{}' owner CPU {} does not match configured {}",
          connection_.name, info.owner_thread_cpu, connection_.worker_cpu_id);
      return false;
    }
    return true;
  }

 private:
  const probe::ProbeConfig& config_;
  probe::ProbeConnectionConfig connection_;
  bitget::OrderSessionConfig order_session_config_;
  aquila::config::InstrumentCatalog instrument_catalog_;
  ProbeDataReader data_reader_;
  probe::LocalFeedbackQueue feedback_queue_;
  ProbeResponseHandler handler_;
  Session session_;
  Runner runner_;
  std::atomic<bool> returned_{false};
  bool start_result_{false};
  std::thread thread_;
};

[[nodiscard]] std::unique_ptr<LiveSessionStateBase> MakeSessionState(
    const probe::ProbeConfig& config,
    const probe::ProbeConnectionConfig& connection, std::size_t session_index,
    std::size_t session_count,
    const bitget::OrderSessionConfig& order_session_config,
    const bitget::LoginCredentials& credentials,
    aquila::config::DataReaderConfig data_reader_config,
    probe::SampleCsvWriter& sample_writer) {
  if (connection.enable_tls) {
    return std::make_unique<
        LiveSessionState<bitget::OrderSessionDefaultTlsWebSocketPolicy>>(
        config, connection, session_index, session_count, order_session_config,
        credentials, std::move(data_reader_config), sample_writer);
  }
  return std::make_unique<
      LiveSessionState<bitget::OrderSessionDefaultPlainWebSocketPolicy>>(
      config, connection, session_index, session_count, order_session_config,
      credentials, std::move(data_reader_config), sample_writer);
}

struct ExecuteSummary {
  std::uint64_t orders_sent{0};
  std::uint64_t samples_completed{0};
  std::uint64_t samples_failed{0};
};

[[nodiscard]] ExecuteSummary Summarize(
    const std::vector<std::unique_ptr<LiveSessionStateBase>>& sessions) {
  ExecuteSummary summary;
  for (const auto& session : sessions) {
    summary.orders_sent += session->orders_sent();
    summary.samples_completed += session->samples_completed();
    summary.samples_failed += session->samples_failed();
  }
  return summary;
}

void StopAndJoin(
    std::vector<std::unique_ptr<LiveSessionStateBase>>* sessions) noexcept {
  for (auto& session : *sessions) {
    session->StopGracefully();
  }
  for (auto& session : *sessions) {
    session->Join();
  }
}

[[nodiscard]] int RunExecute(
    const probe::ProbeConfig& config, const probe::ProbeRunPlan& plan,
    const bitget::OrderSessionConfig& order_session_config,
    const bitget::LoginCredentials& credentials, double duration_sec) {
  std::string error;
  if (!BindCoordinatorCpu(config.sampling.coordinator_cpu, &error)) {
    NOVA_ERROR("coordinator_affinity_error={}", error);
    return 1;
  }

  aquila::config::OrderFeedbackShmConfigResult feedback_config =
      aquila::config::LoadOrderFeedbackShmConfigFile(
          config.feedback.shm_config);
  if (!feedback_config.ok) {
    NOVA_ERROR("feedback_shm_config_error={}", feedback_config.error);
    return 1;
  }
  auto feedback_manager = aquila::OrderFeedbackShmManager::Open(
      ToOpenOnlyShmConfig(feedback_config.value));
  if (!feedback_manager.ok) {
    NOVA_ERROR("feedback_shm_open_error={}", feedback_manager.error);
    return 1;
  }
  const std::uint64_t raw_consumer_run_id =
      static_cast<std::uint64_t>(SteadyNowNs());
  auto feedback_reader = aquila::OrderFeedbackShmReader::Claim(
      feedback_manager.value.channel(), config.feedback.strategy_id,
      raw_consumer_run_id == 0 ? 1 : raw_consumer_run_id,
      config.feedback.force_claim);
  if (!feedback_reader.ok) {
    NOVA_ERROR("feedback_reader_claim_error={}", feedback_reader.error);
    return 1;
  }
  std::size_t pending_feedback = 0;
  feedback_reader.value.Poll(
      config.feedback.poll_budget,
      [&pending_feedback](const aquila::OrderFeedbackEvent&) noexcept {
        ++pending_feedback;
      });
  if (pending_feedback != 0) {
    NOVA_ERROR(
        "feedback_lane_not_clean pending_events={} orders_sent=0 "
        "rest_guard_implemented=false",
        pending_feedback);
    return 1;
  }

  const std::filesystem::path run_dir = config.output.root_dir / config.run_id;
  const std::filesystem::path sample_csv_path =
      run_dir / "order_session_rtt_samples.csv";
  const std::filesystem::path connection_csv_path =
      run_dir / "order_session_rtt_connections_observed.csv";
  const std::filesystem::path metadata_path =
      run_dir / "order_session_rtt_run_metadata.json";

  probe::SampleCsvWriter sample_writer;
  if (!sample_writer.Open(sample_csv_path, &error)) {
    NOVA_ERROR("sample_csv_open_error={}", error);
    return 1;
  }
  probe::ConnectionObservedCsvWriter connection_writer;
  if (!connection_writer.Open(connection_csv_path, &error)) {
    NOVA_ERROR("connection_csv_open_error={}", error);
    return 1;
  }
  if (!probe::WriteRunMetadata(
          metadata_path, config, plan.connection_count,
          sample_csv_path.string(), connection_csv_path.string(),
          order_session_config.client_oid_run_namespace.View(), &error)) {
    NOVA_ERROR("run_metadata_error={}", error);
    return 1;
  }

  const std::vector<probe::ProbeConnectionConfig>& connections =
      plan.cycles.front().connections;
  std::vector<std::unique_ptr<LiveSessionStateBase>> sessions;
  sessions.reserve(connections.size());
  for (std::size_t i = 0; i < connections.size(); ++i) {
    aquila::config::DataReaderConfigResult data_reader_config =
        aquila::config::LoadDataReaderConfigFile(
            config.inputs.data_reader_config);
    if (!data_reader_config.ok) {
      NOVA_ERROR("data_reader_config_error={}", data_reader_config.error);
      return 1;
    }
    sessions.push_back(MakeSessionState(
        config, connections[i], i, connections.size(), order_session_config,
        credentials, std::move(data_reader_config.value), sample_writer));
  }
  std::vector<probe::LocalFeedbackQueue*> queues;
  queues.reserve(sessions.size());
  for (auto& session : sessions) {
    queues.push_back(session->feedback_queue());
  }

  for (auto& session : sessions) {
    if (!session->StartThread(&error)) {
      NOVA_ERROR("session_start_error={}", error);
      StopAndJoin(&sessions);
      return 1;
    }
  }

  const std::int64_t start_ns = SteadyNowNs();
  const std::int64_t duration_ns = static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::duration<double>(duration_sec))
          .count());
  const std::int64_t hard_deadline_ns = start_ns + duration_ns;
  const std::int64_t login_deadline_ns = std::min(
      hard_deadline_ns, start_ns + static_cast<std::int64_t>(
                                       config.sessions.wait_login_timeout_ms) *
                                       1'000'000);
  bool startup_ok = false;
  while (SteadyNowNs() < login_deadline_ns) {
    if (g_stop_requested != 0) {
      error = "signal requested stop while waiting for login";
      break;
    }
    const probe::RouteFeedbackResult routed = probe::RouteFeedback(
        feedback_reader.value, queues, config.feedback.poll_budget);
    if (routed.failed) {
      error = "feedback routing failed while waiting for login";
      break;
    }
    bool all_ready = true;
    bool any_failed = false;
    for (const auto& session : sessions) {
      all_ready = all_ready && session->login_ready();
      any_failed = any_failed || session->failed() ||
                   (session->returned() && !session->login_ready());
    }
    if (any_failed) {
      error = "order session failed before all logins became ready";
      break;
    }
    if (all_ready) {
      startup_ok = true;
      break;
    }
    std::this_thread::yield();
  }
  if (!startup_ok) {
    if (error.empty()) {
      error = "wait login timeout";
    }
    NOVA_ERROR("execute_startup_error={} orders_sent=0", error);
    StopAndJoin(&sessions);
    return 1;
  }

  for (const auto& session : sessions) {
    if (!session->WriteConnectionObservation(&connection_writer, &error)) {
      NOVA_ERROR("connection_observation_error={} orders_sent=0", error);
      StopAndJoin(&sessions);
      return 1;
    }
  }

  NOVA_INFO(
      "bitget_order_session_rtt_probe execute=true run_id={} symbol={} "
      "session_count={} samples_per_session={} duration_sec={:.3f} "
      "client_oid_run_namespace={} "
      "rest_guard_implemented=false dedicated_account_confirmed=true",
      config.run_id, config.order.symbol, sessions.size(),
      config.sampling.samples_per_session, duration_sec,
      order_session_config.client_oid_run_namespace.View());

  probe::SequentialCoordinator coordinator(
      sessions.size(), config.sampling.samples_per_session,
      config.sampling.order_session_interval_us,
      config.sampling.cycle_cooldown_us);
  std::vector<std::uint64_t> observed_finished(sessions.size(), 0);
  std::vector<std::uint64_t> observed_completed(sessions.size(), 0);
  bool run_failed = false;
  std::string stop_reason;

  while (!coordinator.complete() && !run_failed) {
    const std::int64_t loop_now_ns = SteadyNowNs();
    if (g_stop_requested != 0) {
      run_failed = true;
      stop_reason = "signal requested stop";
      break;
    }
    if (loop_now_ns >= hard_deadline_ns) {
      run_failed = true;
      stop_reason = "duration deadline reached before sample completion";
      break;
    }
    const probe::RouteFeedbackResult routed = probe::RouteFeedback(
        feedback_reader.value, queues, config.feedback.poll_budget);
    if (routed.failed) {
      run_failed = true;
      stop_reason = "feedback routing failed";
      break;
    }

    for (std::size_t i = 0; i < sessions.size(); ++i) {
      if (sessions[i]->failed()) {
        run_failed = true;
        stop_reason = fmt::format("session '{}' failed: {}",
                                  sessions[i]->connection().name,
                                  sessions[i]->failure_reason());
        break;
      }
      if (sessions[i]->returned()) {
        run_failed = true;
        stop_reason = fmt::format("session '{}' runtime returned early",
                                  sessions[i]->connection().name);
        break;
      }
      const std::uint64_t finished = sessions[i]->samples_finished();
      if (finished == observed_finished[i]) {
        continue;
      }
      if (finished != observed_finished[i] + 1) {
        run_failed = true;
        stop_reason = "session completion count jumped unexpectedly";
        break;
      }
      const std::uint64_t completed = sessions[i]->samples_completed();
      const bool success = completed == observed_completed[i] + 1;
      if (!coordinator.MarkSampleFinished(i, success, SteadyNowNs())) {
        run_failed = true;
        stop_reason = "coordinator rejected session completion";
        break;
      }
      observed_finished[i] = finished;
      observed_completed[i] = completed;
      if (!success) {
        run_failed = true;
        stop_reason = "sample failed";
        break;
      }
    }
    if (run_failed || coordinator.complete()) {
      break;
    }

    const std::int64_t now_ns = SteadyNowNs();
    const std::optional<std::size_t> grant = coordinator.NextGrant(now_ns);
    if (grant.has_value()) {
      sessions[*grant]->GrantDispatch();
    } else {
      std::this_thread::yield();
    }
  }

  if (run_failed) {
    const std::int64_t grace_deadline =
        SteadyNowNs() +
        static_cast<std::int64_t>(config.feedback.terminal_timeout_ms) *
            1'000'000;
    while (SteadyNowNs() < grace_deadline) {
      const probe::RouteFeedbackResult grace_routed = probe::RouteFeedback(
          feedback_reader.value, queues, config.feedback.poll_budget);
      (void)grace_routed;
      bool any_active = false;
      for (const auto& session : sessions) {
        any_active = any_active || session->has_active_sample();
      }
      if (!any_active) {
        break;
      }
      std::this_thread::yield();
    }
    for (auto& session : sessions) {
      if (session->has_active_sample()) {
        session->RequestAbort();
      }
    }
    const std::int64_t abort_deadline = SteadyNowNs() + 100'000'000;
    while (SteadyNowNs() < abort_deadline) {
      bool any_active = false;
      for (const auto& session : sessions) {
        any_active = any_active || session->has_active_sample();
      }
      if (!any_active) {
        break;
      }
      std::this_thread::yield();
    }
  }

  StopAndJoin(&sessions);
  sample_writer.Close();
  connection_writer.Close();
  const ExecuteSummary summary = Summarize(sessions);
  const bool success =
      !run_failed && coordinator.complete() && summary.samples_failed == 0;
  NOVA_INFO(
      "bitget_order_session_rtt_probe_summary result={} stop_reason={} "
      "orders_sent={} samples_completed={} samples_failed={} "
      "sample_csv_path={} connection_csv_path={} "
      "rest_guard_implemented=false flat_proven=false",
      success ? "ok" : "failed", success ? "sample_count_reached" : stop_reason,
      summary.orders_sent, summary.samples_completed, summary.samples_failed,
      sample_csv_path.string(), connection_csv_path.string());
  return success ? 0 : 1;
}

[[nodiscard]] int Run(const CliOptions& options, const toml::table& root) {
  probe::ProbeConfigResult config_result = probe::ParseProbeConfig(root);
  if (!config_result.ok) {
    NOVA_ERROR("config_error={}", config_result.error);
    return 1;
  }
  probe::ProbeConfig config = std::move(config_result.value);
  const OverrideResult overrides = ApplyOverrides(options, &config);
  if (!overrides.ok) {
    NOVA_ERROR("option_error={}", overrides.error);
    return 2;
  }
  EnsureRunId(&config);

  probe::ProbeConnectionsCsvResult connections =
      probe::LoadProbeConnectionsCsvFile(config.inputs.connections_file);
  if (!connections.ok) {
    NOVA_ERROR("connections_csv_error={}", connections.error);
    return 1;
  }
  probe::ProbeRunPlanResult plan =
      probe::BuildProbeRunPlan(config, std::move(connections.connections));
  if (!plan.ok) {
    NOVA_ERROR("run_plan_error={}", plan.error);
    return 1;
  }

  if (!options.live_preflight && !options.execute) {
    PrintPlan(config, plan.value, false);
    return 0;
  }

  if (config.sampling.coordinator_cpu >= CPU_SETSIZE) {
    NOVA_ERROR("live_preflight_error=coordinator CPU exceeds CPU_SETSIZE");
    return 1;
  }
  for (const probe::ProbeConnectionConfig& connection :
       plan.value.cycles.front().connections) {
    if (connection.worker_cpu_id >= CPU_SETSIZE) {
      NOVA_ERROR(
          "live_preflight_error=session '{}' worker CPU exceeds CPU_SETSIZE",
          connection.name);
      return 1;
    }
  }

  PreflightResult preflight = ValidateLivePreflight(config);
  if (!preflight.ok) {
    NOVA_ERROR("live_preflight_error={}", preflight.error);
    return 1;
  }
  if (options.live_preflight) {
    PrintPlan(config, plan.value, true);
    NOVA_INFO(
        "bitget_order_session_rtt_probe_preflight symbol={} "
        "order_session_config={} data_reader_config={} feedback_shm_config={} "
        "client_oid_run_namespace={} "
        "credentials_read=false shm_attached=false websocket_created=false "
        "orders_sent=0 rest_guard_implemented=false",
        config.order.symbol, config.inputs.order_session_config.string(),
        config.inputs.data_reader_config.string(),
        config.feedback.shm_config.string(),
        preflight.order_session_config.client_oid_run_namespace.View());
    return 0;
  }

  bitget::LoginCredentials credentials;
  std::string error;
  if (!LoadCredentials(preflight.order_session_config, &credentials, &error)) {
    NOVA_ERROR("credential_error={}", error);
    return 1;
  }
  return RunExecute(config, plan.value, preflight.order_session_config,
                    credentials, options.duration_sec);
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;
  CLI::App app{"Bitget order session IOC RTT probe"};
  app.add_option("--config", options.config_path, "Probe TOML path");
  app.add_option("--connections-file", options.connections_file_override,
                 "Override probe.inputs.connections_file");
  app.add_option("--samples-per-session", options.samples_per_session_override,
                 "Override probe.sampling.samples_per_session");
  app.add_option("--duration-sec", options.duration_sec,
                 "Hard execute deadline in seconds");
  app.add_flag("--live-preflight", options.live_preflight,
               "Validate live inputs without credentials, SHM attach or "
               "network connections");
  app.add_flag("--execute", options.execute, "Enable real IOC order execution");
  app.add_flag("--confirm-dedicated-account", options.confirm_dedicated_account,
               "Confirm the account is dedicated and externally verified "
               "flat");
  CLI11_PARSE(app, argc, argv);

  const probe::ExecuteGuardResult guard =
      probe::ValidateExecuteGuard(probe::ExecuteGuardInput{
          .execute = options.execute,
          .live_preflight = options.live_preflight,
          .confirm_dedicated_account = options.confirm_dedicated_account,
          .duration_sec = options.duration_sec,
      });
  if (!guard.ok) {
    fmt::print(stderr, "option_error={}\n", guard.error);
    return 2;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  try {
    const toml::parse_result root =
        toml::parse_file(options.config_path.string());
    nova::LoggingGuard logging_guard{root};
    NOVA_INFO(
        "bitget_order_session_rtt_probe_start config_path={} execute={} "
        "live_preflight={} confirm_dedicated_account={} duration_sec={:.3f}",
        options.config_path.string(), options.execute ? "true" : "false",
        options.live_preflight ? "true" : "false",
        options.confirm_dedicated_account ? "true" : "false",
        options.duration_sec);
    return Run(options, root);
  } catch (const std::exception& ex) {
    fmt::print(stderr, "config_error={}\n", ex.what());
    return 1;
  }
}
