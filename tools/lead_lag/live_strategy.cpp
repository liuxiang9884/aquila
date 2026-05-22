#include "tools/lead_lag/live_strategy.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/config/strategy_config.h"
#include "core/market_data/realtime_data_reader.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "core/trading/trading_runtime.h"
#include "exchange/gate/trading/order_session_config.h"
#include "exchange/gate/trading/order_session_runtime_adapter.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/signal.h"
#include "strategy/lead_lag/strategy.h"
#include "tools/lead_lag/signal_csv_writer.h"

namespace {

namespace config = aquila::config;
namespace core = aquila::core;
namespace gate = aquila::gate;
namespace leadlag = aquila::strategy::leadlag;
namespace live = aquila::tools::lead_lag;
namespace market_data = aquila::market_data;

struct CliOptions {
  std::filesystem::path config_path{
      "config/strategies/lead_lag_btc_strategy.toml"};
  std::filesystem::path signals_output_path;
  std::string api_key_env;
  std::string api_secret_env;
  std::uint64_t duration_sec{0};
  std::string smoke_symbol;
  double smoke_aggressive_price_bps{100.0};
  double smoke_max_notional{100.0};
  bool connect_data{false};
  bool execute{false};
  bool smoke_open_close{false};
};

struct LoadedConfig {
  config::StrategyConfig strategy;
  config::DataReaderConfig data_reader;
  gate::OrderSessionConfig order_session;
  leadlag::Config lead_lag;
};

struct SignalOnlyStats {
  std::uint64_t book_tickers{0};
  std::uint64_t signals{0};
  std::uint64_t open_signals{0};
  std::uint64_t close_signals{0};
  std::uint64_t stoploss_signals{0};
  std::uint64_t order_responses{0};
  std::uint64_t order_feedbacks{0};
  live::RecoveryDiagnostics recovery;
};

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

struct NullOrderSession {
  enum class SendStatus : std::uint8_t {
    kOk,
    kRejected,
  };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  [[nodiscard]] bool Start() noexcept {
    running = true;
    return true;
  }

  void Stop() noexcept {
    running = false;
  }

  [[nodiscard]] bool Ready() const noexcept {
    return true;
  }

  [[nodiscard]] bool Running() const noexcept {
    return running;
  }

  SendResult PlaceOrder(core::StrategyOrder&) noexcept {
    return {};
  }

  SendResult CancelOrder(core::StrategyOrder&) noexcept {
    return {};
  }

  bool running{false};
};

class SignalOnlyStrategy {
 public:
  SignalOnlyStrategy(leadlag::Config config, SignalOnlyStats* stats,
                     aquila::tools::lead_lag::SignalCsvWriter* signal_writer)
      : inner_(std::move(config),
               leadlag::StrategyOptions{
                   .position_accounting =
                       leadlag::PositionAccountingMode::kSyntheticSignals,
               }),
        stats_(stats),
        signal_writer_(signal_writer) {
    RefreshRecoveryDiagnostics();
  }

  template <typename ContextT>
  void OnBookTicker(const aquila::BookTicker& ticker,
                    ContextT& context) noexcept {
    if (stats_ != nullptr) {
      ++stats_->book_tickers;
    }
    inner_.OnBookTicker(ticker, context);

    const leadlag::SignalDecision& decision = inner_.last_signal_decision();
    if (!decision.triggered) {
      return;
    }
    RecordSignal(decision);
    if (signal_writer_ != nullptr && inner_.last_signal_diagnostics_valid()) {
      signal_writer_->Write(ticker, decision, inner_.last_signal_diagnostics());
    }
  }

  template <typename ContextT>
  void OnOrderResponse(const core::OrderResponseEvent& event,
                       ContextT& context) noexcept {
    if (stats_ != nullptr) {
      ++stats_->order_responses;
    }
    inner_.OnOrderResponse(event, context);
    RefreshRecoveryDiagnostics();
  }

  template <typename ContextT>
  void OnOrderFeedback(const aquila::OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    if (stats_ != nullptr) {
      ++stats_->order_feedbacks;
    }
    inner_.OnOrderFeedback(event, context);
    RefreshRecoveryDiagnostics();
  }

  [[nodiscard]] bool ShouldStop() const noexcept {
    return inner_.ShouldStop();
  }

 private:
  void RefreshRecoveryDiagnostics() noexcept {
    if (stats_ == nullptr) {
      return;
    }
    stats_->recovery = live::RecoveryDiagnostics{
        .recovery_state = inner_.recovery_state(),
        .needs_reconcile = inner_.needs_reconcile(),
        .manual_intervention = inner_.manual_intervention(),
        .new_entries_paused = inner_.new_entries_paused(),
    };
  }

  void RecordSignal(const leadlag::SignalDecision& decision) noexcept {
    if (stats_ == nullptr) {
      return;
    }
    ++stats_->signals;
    switch (decision.action) {
      case leadlag::SignalAction::kOpenLong:
      case leadlag::SignalAction::kOpenShort:
        ++stats_->open_signals;
        break;
      case leadlag::SignalAction::kCloseLong:
      case leadlag::SignalAction::kCloseShort:
        ++stats_->close_signals;
        break;
      case leadlag::SignalAction::kStoplossLong:
      case leadlag::SignalAction::kStoplossShort:
        ++stats_->stoploss_signals;
        break;
      case leadlag::SignalAction::kNone:
        break;
    }
  }

  leadlag::Strategy inner_;
  SignalOnlyStats* stats_{};
  aquila::tools::lead_lag::SignalCsvWriter* signal_writer_{};
};

std::string_view StrategyModeText(config::StrategyMode mode) noexcept {
  switch (mode) {
    case config::StrategyMode::kDryRun:
      return "dry_run";
    case config::StrategyMode::kLive:
      return "live";
  }
  return "unknown";
}

bool ValidateLoadedConfig(const LoadedConfig& loaded) {
  if (loaded.strategy.name != "lead_lag") {
    fmt::print(stderr, "[FAIL] strategy.name must be lead_lag for this tool\n");
    NOVA_ERROR("strategy.name must be lead_lag actual={}",
               loaded.strategy.name);
    return false;
  }
  if (loaded.lead_lag.pairs.empty()) {
    fmt::print(stderr,
               "[FAIL] lead_lag config must contain at least one pair\n");
    NOVA_ERROR("lead_lag config must contain at least one pair");
    return false;
  }
  return true;
}

bool LoadConfig(const CliOptions& options, LoadedConfig* loaded) {
  auto strategy_result = config::LoadStrategyConfigFile(options.config_path);
  if (!strategy_result.ok) {
    fmt::print(stderr, "[FAIL] strategy_config_error={}\n",
               strategy_result.error);
    NOVA_ERROR("strategy_config_error={}", strategy_result.error);
    return false;
  }
  loaded->strategy = std::move(strategy_result.value);

  auto data_reader_result = config::LoadDataReaderConfigFile(
      loaded->strategy.data_reader.config_path);
  if (!data_reader_result.ok) {
    fmt::print(stderr, "[FAIL] data_reader_config_error={}\n",
               data_reader_result.error);
    NOVA_ERROR("data_reader_config_error={}", data_reader_result.error);
    return false;
  }
  loaded->data_reader = std::move(data_reader_result.value);

  auto order_session_result = gate::LoadOrderSessionConfigFile(
      loaded->strategy.order_session.config_path);
  if (!order_session_result.ok) {
    fmt::print(stderr, "[FAIL] order_session_config_error={}\n",
               order_session_result.error);
    NOVA_ERROR("order_session_config_error={}", order_session_result.error);
    return false;
  }
  loaded->order_session = std::move(order_session_result.value);

  auto lead_lag_result =
      leadlag::LoadConfigFile(loaded->strategy.user_config_path,
                              loaded->data_reader.instrument_catalog);
  if (!lead_lag_result.ok) {
    fmt::print(stderr, "[FAIL] lead_lag_config_error={}\n",
               lead_lag_result.error);
    NOVA_ERROR("lead_lag_config_error={}", lead_lag_result.error);
    return false;
  }
  loaded->lead_lag = std::move(lead_lag_result.value);

  return ValidateLoadedConfig(*loaded);
}

gate::LoginCredentials LoadCredentials(const CliOptions& options,
                                       const gate::OrderSessionConfig& config,
                                       bool* ok) {
  const std::string api_key_env = options.api_key_env.empty()
                                      ? config.credentials.api_key_env
                                      : options.api_key_env;
  const std::string api_secret_env = options.api_secret_env.empty()
                                         ? config.credentials.api_secret_env
                                         : options.api_secret_env;
  const char* api_key = EnvValue(api_key_env);
  if (api_key == nullptr) {
    fmt::print(stderr, "[FAIL] missing env var {}\n", api_key_env);
    NOVA_ERROR("missing env var {}", api_key_env);
    *ok = false;
    return {};
  }
  const char* api_secret = EnvValue(api_secret_env);
  if (api_secret == nullptr) {
    fmt::print(stderr, "[FAIL] missing env var {}\n", api_secret_env);
    NOVA_ERROR("missing env var {}", api_secret_env);
    *ok = false;
    return {};
  }
  *ok = true;
  return gate::LoginCredentials{.api_key = api_key, .api_secret = api_secret};
}

void ApplyDurationOverride(const CliOptions& options, LoadedConfig* loaded) {
  if (options.duration_sec != 0) {
    loaded->strategy.loop.max_loop_seconds = options.duration_sec;
  }
}

void PrintLoadedConfigSummary(const CliOptions& options,
                              const LoadedConfig& loaded, live::RunMode mode) {
  fmt::print(
      "lead_lag_strategy run_mode={} execute={} connect_data={} config={} "
      "signals_output={} strategy_name={} mode={} strategy_id={} "
      "order_capacity={} max_loop_seconds={} reader_name={} sources={} "
      "order_session={} feedback_enabled={} feedback_shm={} "
      "feedback_channel={} feedback_poll_budget={} pairs={} "
      "smoke_open_close={} smoke_symbol={} smoke_aggressive_price_bps={} "
      "smoke_max_notional={}\n",
      live::RunModeName(mode), options.execute ? "true" : "false",
      options.connect_data ? "true" : "false", options.config_path.string(),
      options.signals_output_path.empty()
          ? "-"
          : options.signals_output_path.string(),
      loaded.strategy.name, StrategyModeText(loaded.strategy.mode),
      loaded.strategy.strategy_id, loaded.strategy.order_capacity,
      loaded.strategy.loop.max_loop_seconds, loaded.data_reader.name,
      loaded.data_reader.sources.size(), loaded.order_session.name,
      loaded.strategy.feedback.enabled ? "true" : "false",
      loaded.strategy.feedback.shm_name, loaded.strategy.feedback.channel_name,
      loaded.strategy.feedback.poll_budget, loaded.lead_lag.pairs.size(),
      options.smoke_open_close ? "true" : "false",
      options.smoke_symbol.empty() ? "-" : options.smoke_symbol,
      options.smoke_aggressive_price_bps, options.smoke_max_notional);
}

int RunValidateOnly() {
  fmt::print(
      "lead_lag_strategy_validate_only dry_run=true no websocket connection "
      "opened no shm opened\n");
  NOVA_INFO(
      "lead_lag_strategy_validate_only dry_run=true no websocket connection "
      "opened no shm opened");
  return 0;
}

int RunSignalOnly(LoadedConfig loaded, const CliOptions& options) {
  using DataReader = market_data::RealtimeDataReader<
      market_data::RealtimeDataReaderDiagnostics>;
  using Runtime =
      core::TradingRuntime<SignalOnlyStrategy, NullOrderSession, DataReader,
                           core::TradingRuntimeDiagnostics>;

  loaded.strategy.feedback.enabled = false;

  SignalOnlyStats stats;
  aquila::tools::lead_lag::SignalCsvWriter signal_writer;
  aquila::tools::lead_lag::SignalCsvWriter* signal_writer_ptr = nullptr;
  if (!options.signals_output_path.empty()) {
    std::string error;
    if (!signal_writer.Open(options.signals_output_path, &error)) {
      fmt::print(stderr, "[FAIL] signals_output_error={}\n", error);
      return 1;
    }
    signal_writer_ptr = &signal_writer;
  }

  auto runtime_result = Runtime::Create(
      std::move(loaded.strategy), std::move(loaded.data_reader),
      [] { return NullOrderSession{}; }, std::move(loaded.lead_lag), &stats,
      signal_writer_ptr);
  if (!runtime_result.ok) {
    fmt::print(stderr, "[FAIL] runtime_create_error={}\n",
               runtime_result.error);
    NOVA_ERROR("runtime_create_error={}", runtime_result.error);
    return 1;
  }

  const int exit_code = runtime_result.value->Run();
  const core::TradingRuntimeLoopStats& loop_stats =
      runtime_result.value->diagnostics().stats();
  const std::string recovery_fields =
      live::FormatRecoveryDiagnosticsFields(stats.recovery);
  signal_writer.Close();

  fmt::print(
      "lead_lag_strategy_signal_only_summary exit_code={} book_tickers={} "
      "signals={} open={} close={} stoploss={} order_responses={} "
      "order_feedbacks={} {} loop_iterations={} idle_iterations={} "
      "data_reader_polls={} data_reader_empty_polls={} data_reader_events={} "
      "signals_output={}\n",
      exit_code, stats.book_tickers, stats.signals, stats.open_signals,
      stats.close_signals, stats.stoploss_signals, stats.order_responses,
      stats.order_feedbacks, recovery_fields, loop_stats.loop_iterations,
      loop_stats.idle_iterations, loop_stats.data_reader_poll_calls,
      loop_stats.data_reader_empty_polls, loop_stats.data_reader_events,
      options.signals_output_path.empty()
          ? "-"
          : options.signals_output_path.string());
  return exit_code;
}

template <typename WebSocketPolicy>
int RunLiveOrdersRuntime(LoadedConfig loaded,
                         gate::LoginCredentials credentials) {
  using DataReader = market_data::RealtimeDataReader<
      market_data::RealtimeDataReaderDiagnostics>;
  using OrderSession =
      gate::OrderSessionRuntimeAdapter<WebSocketPolicy,
                                       gate::OrderSessionDiagnostics>;
  using Runtime =
      core::TradingRuntime<live::LiveOrdersStrategy, OrderSession, DataReader,
                           core::TradingRuntimeDiagnostics>;

  live::LiveOrdersStrategyStats stats;
  gate::OrderSessionConfig order_session_config =
      std::move(loaded.order_session);
  auto runtime_result = Runtime::Create(
      std::move(loaded.strategy), std::move(loaded.data_reader),
      [order_session_config = std::move(order_session_config),
       credentials = std::move(credentials)]() mutable {
        return OrderSession(std::move(order_session_config),
                            std::move(credentials));
      },
      std::move(loaded.lead_lag), &stats);
  if (!runtime_result.ok) {
    fmt::print(stderr, "[FAIL] runtime_create_error={}\n",
               runtime_result.error);
    NOVA_ERROR("runtime_create_error={}", runtime_result.error);
    return 1;
  }

  const int runtime_exit_code = runtime_result.value->Run();
  const int exit_code =
      live::ResolveLiveOrdersExitCode(runtime_exit_code, stats);
  const core::TradingRuntimeLoopStats& loop_stats =
      runtime_result.value->diagnostics().stats();
  const std::string recovery_fields =
      live::FormatRecoveryDiagnosticsFields(stats.recovery);

  if (stats.continuity_lost_stop_requested) {
    fmt::print(stderr,
               "[FAIL] lead_lag live order feedback continuity lost; "
               "runtime stopped; run emergency flatten before restart\n");
    NOVA_ERROR(
        "lead_lag live order feedback continuity lost; runtime stopped; "
        "run emergency flatten before restart");
  }

  fmt::print(
      "lead_lag_strategy_live_orders_summary exit_code={} "
      "runtime_exit_code={} emergency_handoff={} book_tickers={} "
      "order_responses={} order_feedbacks={} {} loop_iterations={} "
      "idle_iterations={} order_response_polls={} order_response_events={} "
      "order_feedback_polls={} order_feedback_events={} data_reader_polls={} "
      "data_reader_empty_polls={} data_reader_events={}\n",
      exit_code, runtime_exit_code,
      stats.continuity_lost_stop_requested ? "true" : "false",
      stats.book_tickers, stats.order_responses, stats.order_feedbacks,
      recovery_fields, loop_stats.loop_iterations, loop_stats.idle_iterations,
      loop_stats.order_response_poll_calls, loop_stats.order_response_events,
      loop_stats.order_feedback_poll_calls, loop_stats.order_feedback_events,
      loop_stats.data_reader_poll_calls, loop_stats.data_reader_empty_polls,
      loop_stats.data_reader_events);
  return exit_code;
}

int RunLiveOrders(LoadedConfig loaded, const CliOptions& options) {
  bool credentials_ok = false;
  gate::LoginCredentials credentials =
      LoadCredentials(options, loaded.order_session, &credentials_ok);
  if (!credentials_ok) {
    return 2;
  }

  if (loaded.order_session.connection.enable_tls) {
    return RunLiveOrdersRuntime<gate::OrderSessionDefaultTlsWebSocketPolicy>(
        std::move(loaded), std::move(credentials));
  }
  return RunLiveOrdersRuntime<gate::OrderSessionDefaultPlainWebSocketPolicy>(
      std::move(loaded), std::move(credentials));
}

template <typename WebSocketPolicy>
int RunLiveOpenCloseSmokeRuntime(LoadedConfig loaded,
                                 gate::LoginCredentials credentials,
                                 const CliOptions& options) {
  using DataReader = market_data::RealtimeDataReader<
      market_data::RealtimeDataReaderDiagnostics>;
  using OrderSession =
      gate::OrderSessionRuntimeAdapter<WebSocketPolicy,
                                       gate::OrderSessionDiagnostics>;
  using Runtime =
      core::TradingRuntime<live::LiveOpenCloseSmokeStrategy, OrderSession,
                           DataReader, core::TradingRuntimeDiagnostics>;

  live::LiveOpenCloseSmokeStats stats;
  gate::OrderSessionConfig order_session_config =
      std::move(loaded.order_session);
  live::LiveOpenCloseSmokeOptions smoke_options{
      .symbol = options.smoke_symbol,
      .aggressive_price_bps = options.smoke_aggressive_price_bps,
      .max_notional = options.smoke_max_notional,
  };
  auto runtime_result = Runtime::Create(
      std::move(loaded.strategy), std::move(loaded.data_reader),
      [order_session_config = std::move(order_session_config),
       credentials = std::move(credentials)]() mutable {
        return OrderSession(std::move(order_session_config),
                            std::move(credentials));
      },
      std::move(loaded.lead_lag), std::move(smoke_options), &stats);
  if (!runtime_result.ok) {
    fmt::print(stderr, "[FAIL] runtime_create_error={}\n",
               runtime_result.error);
    NOVA_ERROR("runtime_create_error={}", runtime_result.error);
    return 1;
  }

  const int runtime_exit_code = runtime_result.value->Run();
  const int exit_code =
      live::ResolveLiveOpenCloseSmokeExitCode(runtime_exit_code, stats);
  const core::TradingRuntimeLoopStats& loop_stats =
      runtime_result.value->diagnostics().stats();

  if (stats.continuity_lost_stop_requested) {
    fmt::print(stderr,
               "[FAIL] lead_lag smoke order feedback continuity lost; "
               "runtime stopped; run emergency flatten before restart\n");
    NOVA_ERROR(
        "lead_lag smoke order feedback continuity lost; runtime stopped; "
        "run emergency flatten before restart");
  } else if (exit_code != 0) {
    const std::string_view error_text =
        stats.error.empty() ? std::string_view{"smoke did not complete"}
                            : std::string_view{stats.error};
    fmt::print(stderr, "[FAIL] lead_lag smoke open/close failed: {}\n",
               error_text);
    NOVA_ERROR("lead_lag smoke open/close failed: {}", error_text);
  }

  fmt::print(
      "lead_lag_strategy_live_open_close_smoke_summary exit_code={} "
      "runtime_exit_code={} emergency_handoff={} completed={} state={} "
      "book_tickers={} order_responses={} order_feedbacks={} "
      "open_local_order_id={} close_local_order_id={} open_quantity={} "
      "close_quantity={} target_notional={} estimated_open_notional={} "
      "used_min_quantity={} error={} loop_iterations={} idle_iterations={} "
      "order_response_polls={} order_response_events={} "
      "order_feedback_polls={} order_feedback_events={} data_reader_polls={} "
      "data_reader_empty_polls={} data_reader_events={}\n",
      exit_code, runtime_exit_code,
      stats.continuity_lost_stop_requested ? "true" : "false",
      stats.completed ? "true" : "false",
      live::LiveOpenCloseSmokeStateName(stats.state), stats.book_tickers,
      stats.order_responses, stats.order_feedbacks, stats.open_local_order_id,
      stats.close_local_order_id, stats.open_quantity, stats.close_quantity,
      stats.target_notional, stats.estimated_open_notional,
      stats.used_min_quantity ? "true" : "false",
      stats.error.empty() ? "-" : stats.error, loop_stats.loop_iterations,
      loop_stats.idle_iterations, loop_stats.order_response_poll_calls,
      loop_stats.order_response_events, loop_stats.order_feedback_poll_calls,
      loop_stats.order_feedback_events, loop_stats.data_reader_poll_calls,
      loop_stats.data_reader_empty_polls, loop_stats.data_reader_events);
  return exit_code;
}

int RunLiveOpenCloseSmoke(LoadedConfig loaded, const CliOptions& options) {
  bool credentials_ok = false;
  gate::LoginCredentials credentials =
      LoadCredentials(options, loaded.order_session, &credentials_ok);
  if (!credentials_ok) {
    return 2;
  }

  if (loaded.order_session.connection.enable_tls) {
    return RunLiveOpenCloseSmokeRuntime<
        gate::OrderSessionDefaultTlsWebSocketPolicy>(
        std::move(loaded), std::move(credentials), options);
  }
  return RunLiveOpenCloseSmokeRuntime<
      gate::OrderSessionDefaultPlainWebSocketPolicy>(
      std::move(loaded), std::move(credentials), options);
}

int Run(const CliOptions& options) {
  LoadedConfig loaded;
  if (!LoadConfig(options, &loaded)) {
    return 1;
  }
  ApplyDurationOverride(options, &loaded);

  const live::RunModeResult mode_result = live::ResolveRunMode(
      loaded.strategy.mode, options.connect_data, options.execute,
      options.smoke_open_close);
  if (!mode_result.ok) {
    fmt::print(stderr, "[FAIL] {}\n", mode_result.error);
    NOVA_ERROR("{}", mode_result.error);
    return 2;
  }

  PrintLoadedConfigSummary(options, loaded, mode_result.mode);
  switch (mode_result.mode) {
    case live::RunMode::kValidateOnly:
      return RunValidateOnly();
    case live::RunMode::kSignalOnly:
      return RunSignalOnly(std::move(loaded), options);
    case live::RunMode::kLiveOrders:
      return RunLiveOrders(std::move(loaded), options);
    case live::RunMode::kLiveOpenCloseSmoke:
      return RunLiveOpenCloseSmoke(std::move(loaded), options);
  }
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;

  CLI::App app{
      "Run LeadLag live validation or signal-only realtime SHM observation"};
  app.add_option("--config", options.config_path, "Trading runtime TOML path");
  app.add_option("--duration-sec", options.duration_sec,
                 "Override strategy.loop.max_loop_seconds; 0 uses config");
  app.add_option("--signals-output", options.signals_output_path,
                 "Optional CSV path for triggered signal-only signals");
  app.add_option("--api-key", options.api_key_env,
                 "Live order mode API key env override");
  app.add_option("--api-secret", options.api_secret_env,
                 "Live order mode API secret env override");
  app.add_option("--smoke-symbol", options.smoke_symbol,
                 "LeadLag symbol for --smoke-open-close; empty uses first pair");
  app.add_option("--smoke-aggressive-price-bps",
                 options.smoke_aggressive_price_bps,
                 "Open smoke IOC buy limit price offset from ask, in bps");
  app.add_option("--smoke-max-notional", options.smoke_max_notional,
                 "Maximum estimated notional when smoke uses min quantity");
  app.add_flag("--connect-data", options.connect_data,
               "Open realtime SHM data reader and run signal-only");
  app.add_flag("--execute", options.execute,
               "Enable live order mode; requires strategy.mode=live");
  app.add_flag("--smoke-open-close", options.smoke_open_close,
               "Run one controlled open/close live smoke; requires --execute");
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::parse_result toml =
        toml::parse_file(options.config_path.string());
    nova::LoggingGuard logging_guard{toml};
    return Run(options);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "[FAIL] lead_lag_strategy_error={}\n", exc.what());
    return 1;
  }
}
