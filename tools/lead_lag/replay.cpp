#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/config/strategy_config.h"
#include "core/market_data/historical_data_reader.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "core/trading/trading_runtime.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/config.h"
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
#include "strategy/lead_lag/market_calc_csv_writer.h"
#endif
#include "strategy/lead_lag/signal.h"
#include "strategy/lead_lag/signal_csv_writer.h"
#include "strategy/lead_lag/strategy.h"
#include "tools/lead_lag/lag_vol_guard_audit.h"

namespace {

namespace config = aquila::config;
namespace leadlag = aquila::strategy::leadlag;
namespace leadlag_tools = aquila::tools::leadlag;
namespace market_data = aquila::market_data;
namespace core = aquila::core;

struct CliOptions {
  std::filesystem::path config_path{
      "config/strategies/lead_lag_ordi_replay.toml"};
  std::filesystem::path data_reader_config_path;
  std::filesystem::path signals_output_path;
  std::filesystem::path lag_vol_guard_audit_output_path;
  double lag_vol_guard_jump_threshold{0.005};
  std::uint32_t lag_vol_guard_jump_count{3};
  std::string lag_vol_guard_jump_window{"5m"};
  double lag_vol_guard_amplitude_threshold{0.025};
  std::string lag_vol_guard_amplitude_window{"1s"};
  std::string lag_vol_guard_cooldown{"15m"};
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  std::string diagnostic_mode;
  std::filesystem::path market_calc_output_dir;
#endif
};

struct ReplayStats {
  std::uint64_t book_tickers{0};
  std::uint64_t signals{0};
  std::uint64_t open_signals{0};
  std::uint64_t close_signals{0};
  std::uint64_t stoploss_signals{0};
};

struct LoadedConfig {
  config::StrategyConfig strategy_config;
  config::DataReaderConfig data_reader_config;
  leadlag::Config lead_lag_config;
};

struct NullOrderSession {
  enum class SendStatus : std::uint8_t {
    kOk,
    kRejected,
  };

  struct SendResult {
    SendStatus status{SendStatus::kOk};
  };

  [[nodiscard]] bool Start() noexcept {
    return true;
  }

  void Stop() noexcept {}

  [[nodiscard]] bool Ready() const noexcept {
    return true;
  }

  [[nodiscard]] bool Running() const noexcept {
    return true;
  }

  SendResult PlaceOrder(core::StrategyOrder&) noexcept {
    return {};
  }

  SendResult CancelOrder(core::StrategyOrder&) noexcept {
    return {};
  }
};

#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
void WriteMarketCalcRow(void* context,
                        const leadlag::MarketCalcRow& row) noexcept;
#endif

class ReplayStrategy {
 public:
  ReplayStrategy(leadlag::Config config, leadlag::StrategyOptions options,
                 ReplayStats* stats, leadlag::SignalCsvWriter* signal_writer,
                 leadlag_tools::LagVolGuardAuditCollector* lag_vol_audit,
                 leadlag_tools::LagVolGuardAuditCsvWriter*
                     lag_vol_audit_writer
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
                 ,
                 leadlag::MarketCalcCsvWriter* market_calc_writer
#endif
                 )
      : inner_(std::move(config), options),
        stats_(stats),
        signal_writer_(signal_writer),
        lag_vol_audit_(lag_vol_audit),
        lag_vol_audit_writer_(lag_vol_audit_writer) {
    BindTriggeredSignalObserver();
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
    if (market_calc_writer != nullptr) {
      inner_.SetMarketCalcObserver(market_calc_writer, WriteMarketCalcRow);
    }
#endif
  }

  ReplayStrategy(const ReplayStrategy&) = delete;
  ReplayStrategy& operator=(const ReplayStrategy&) = delete;
  ReplayStrategy(ReplayStrategy&& other) noexcept
      : inner_(std::move(other.inner_)),
        stats_(other.stats_),
        signal_writer_(other.signal_writer_),
        lag_vol_audit_(other.lag_vol_audit_),
        lag_vol_audit_writer_(other.lag_vol_audit_writer_),
        stop_requested_(other.stop_requested_) {
    BindTriggeredSignalObserver();
  }
  ReplayStrategy& operator=(ReplayStrategy&& other) noexcept {
    if (this != &other) {
      inner_ = std::move(other.inner_);
      stats_ = other.stats_;
      signal_writer_ = other.signal_writer_;
      lag_vol_audit_ = other.lag_vol_audit_;
      lag_vol_audit_writer_ = other.lag_vol_audit_writer_;
      stop_requested_ = other.stop_requested_;
      BindTriggeredSignalObserver();
    }
    return *this;
  }

  template <typename ContextT>
  void OnBookTicker(const aquila::BookTicker& ticker,
                    ContextT& context) noexcept {
    if (stats_ != nullptr) {
      ++stats_->book_tickers;
    }

    if (lag_vol_audit_ != nullptr) {
      lag_vol_audit_->OnBookTicker(ticker);
    }
    inner_.OnBookTicker(ticker, context);
  }

  template <typename ContextT>
  void OnOrderResponse(const core::OrderResponseEvent& event,
                       ContextT& context) noexcept {
    inner_.OnOrderResponse(event, context);
  }

  template <typename ContextT>
  void OnOrderFeedback(const aquila::OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    inner_.OnOrderFeedback(event, context);
  }

  template <typename ContextT>
  void OnIdle(ContextT&) noexcept {
    stop_requested_ = true;
    inner_.RequestStop();
  }

  [[nodiscard]] bool ShouldStop() const noexcept {
    return stop_requested_ || inner_.ShouldStop();
  }

 private:
  void BindTriggeredSignalObserver() noexcept {
    inner_.SetTriggeredSignalObserver(this, RecordTriggeredSignalCallback);
  }

  static void RecordTriggeredSignalCallback(
      void* context, const aquila::BookTicker& trigger_ticker,
      const leadlag::SignalDecision& decision,
      const leadlag::SignalDiagnostics& diagnostics) noexcept {
    auto* replay = static_cast<ReplayStrategy*>(context);
    if (replay != nullptr) {
      replay->RecordTriggeredSignal(trigger_ticker, decision, diagnostics);
    }
  }

  void RecordTriggeredSignal(
      const aquila::BookTicker& trigger_ticker,
      const leadlag::SignalDecision& decision,
      const leadlag::SignalDiagnostics& diagnostics) noexcept {
    RecordSignal(decision);
    if (signal_writer_ != nullptr) {
      signal_writer_->Write(trigger_ticker, decision, diagnostics);
    }
    if (lag_vol_audit_ != nullptr && lag_vol_audit_writer_ != nullptr) {
      leadlag_tools::LagVolGuardAuditRow row;
      if (lag_vol_audit_->BuildOpenSignalRow(trigger_ticker, decision,
                                             diagnostics, &row)) {
        lag_vol_audit_writer_->Write(row);
      }
    }
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
  ReplayStats* stats_{};
  leadlag::SignalCsvWriter* signal_writer_{};
  leadlag_tools::LagVolGuardAuditCollector* lag_vol_audit_{nullptr};
  leadlag_tools::LagVolGuardAuditCsvWriter* lag_vol_audit_writer_{nullptr};
  bool stop_requested_{false};
};

#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
void WriteMarketCalcRow(void* context,
                        const leadlag::MarketCalcRow& row) noexcept {
  auto* writer = static_cast<leadlag::MarketCalcCsvWriter*>(context);
  if (writer != nullptr) {
    writer->Write(row);
  }
}

bool MarketCalcDiagnosticMode(const CliOptions& options) noexcept {
  return options.diagnostic_mode == "market_calc";
}
#endif

std::string_view ModeText(config::StrategyMode mode) noexcept {
  switch (mode) {
    case config::StrategyMode::kDryRun:
      return "dry_run";
    case config::StrategyMode::kLive:
      return "live";
  }
  return "unknown";
}

std::uint64_t CountInputFiles(const config::DataReaderConfig& config) noexcept {
  std::uint64_t files = 0;
  for (const config::DataReaderSourceConfig& source : config.sources) {
    files += static_cast<std::uint64_t>(source.files.size());
  }
  return files;
}

bool ValidateLoadedConfig(const LoadedConfig& loaded) {
  if (loaded.strategy_config.name != "lead_lag") {
    fmt::print(stderr,
               "[FAIL] strategy.name must be lead_lag for replay tool\n");
    NOVA_ERROR("strategy.name must be lead_lag actual={}",
               loaded.strategy_config.name);
    return false;
  }
  if (loaded.strategy_config.feedback.enabled) {
    fmt::print(stderr,
               "[FAIL] strategy.feedback.enabled must be false for replay\n");
    NOVA_ERROR("strategy.feedback.enabled must be false for replay");
    return false;
  }
  if (loaded.lead_lag_config.pairs.empty()) {
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
  loaded->strategy_config = std::move(strategy_result.value);

  if (!options.data_reader_config_path.empty()) {
    loaded->strategy_config.data_reader.config_path =
        options.data_reader_config_path;
  }

  auto data_reader_result = config::LoadDataReaderConfigFile(
      loaded->strategy_config.data_reader.config_path);
  if (!data_reader_result.ok) {
    fmt::print(stderr, "[FAIL] data_reader_config_error={}\n",
               data_reader_result.error);
    NOVA_ERROR("data_reader_config_error={}", data_reader_result.error);
    return false;
  }
  loaded->data_reader_config = std::move(data_reader_result.value);

  auto lead_lag_result =
      leadlag::LoadConfigFile(loaded->strategy_config.user_config_path,
                              loaded->data_reader_config.instrument_catalog);
  if (!lead_lag_result.ok) {
    fmt::print(stderr, "[FAIL] lead_lag_config_error={}\n",
               lead_lag_result.error);
    NOVA_ERROR("lead_lag_config_error={}", lead_lag_result.error);
    return false;
  }
  loaded->lead_lag_config = std::move(lead_lag_result.value);

  return ValidateLoadedConfig(*loaded);
}

bool BuildLagVolGuardAuditConfig(
    const CliOptions& options,
    leadlag_tools::LagVolGuardAuditConfig* config) {
  if (config == nullptr) {
    return false;
  }
  if (options.lag_vol_guard_jump_threshold <= 0.0) {
    fmt::print(stderr,
               "[FAIL] --lag-vol-guard-jump-threshold must be positive\n");
    NOVA_ERROR("lag_vol_guard_jump_threshold_error value={}",
               options.lag_vol_guard_jump_threshold);
    return false;
  }
  if (options.lag_vol_guard_jump_count == 0) {
    fmt::print(stderr, "[FAIL] --lag-vol-guard-jump-count must be positive\n");
    NOVA_ERROR("lag_vol_guard_jump_count_error value={}",
               options.lag_vol_guard_jump_count);
    return false;
  }
  if (options.lag_vol_guard_amplitude_threshold <= 0.0) {
    fmt::print(stderr,
               "[FAIL] --lag-vol-guard-amplitude-threshold must be positive\n");
    NOVA_ERROR("lag_vol_guard_amplitude_threshold_error value={}",
               options.lag_vol_guard_amplitude_threshold);
    return false;
  }

  std::string error;
  std::uint64_t jump_window_ns = 0;
  if (!leadlag_tools::ParseLagVolGuardAuditDurationNs(
          options.lag_vol_guard_jump_window, &jump_window_ns, &error)) {
    fmt::print(stderr, "[FAIL] --lag-vol-guard-jump-window {}\n", error);
    NOVA_ERROR("lag_vol_guard_jump_window_error value={} error={}",
               options.lag_vol_guard_jump_window, error);
    return false;
  }

  std::uint64_t amplitude_window_ns = 0;
  if (!leadlag_tools::ParseLagVolGuardAuditDurationNs(
          options.lag_vol_guard_amplitude_window, &amplitude_window_ns,
          &error)) {
    fmt::print(stderr, "[FAIL] --lag-vol-guard-amplitude-window {}\n", error);
    NOVA_ERROR("lag_vol_guard_amplitude_window_error value={} error={}",
               options.lag_vol_guard_amplitude_window, error);
    return false;
  }

  std::uint64_t cooldown_ns = 0;
  if (!leadlag_tools::ParseLagVolGuardAuditDurationNs(
          options.lag_vol_guard_cooldown, &cooldown_ns, &error)) {
    fmt::print(stderr, "[FAIL] --lag-vol-guard-cooldown {}\n", error);
    NOVA_ERROR("lag_vol_guard_cooldown_error value={} error={}",
               options.lag_vol_guard_cooldown, error);
    return false;
  }

  *config = leadlag_tools::LagVolGuardAuditConfig{
      .jump_threshold = options.lag_vol_guard_jump_threshold,
      .jump_count = options.lag_vol_guard_jump_count,
      .jump_window_ns = jump_window_ns,
      .amplitude_threshold = options.lag_vol_guard_amplitude_threshold,
      .amplitude_window_ns = amplitude_window_ns,
      .cooldown_ns = cooldown_ns,
  };
  return true;
}

void PrintLoadedConfigSummary(const LoadedConfig& loaded,
                              const CliOptions& options) {
  fmt::print(
      "lead_lag_replay config={} data_reader_config={} "
      "signals_output={} lag_vol_guard_audit_output={}"
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      " diagnostic_mode={} market_calc_output_dir={}"
#endif
      " strategy_name={} mode={} strategy_id={} "
      "order_capacity={} reader_name={} sources={} files={} pairs={}\n",
      options.config_path.string(),
      loaded.strategy_config.data_reader.config_path.string(),
      options.signals_output_path.empty() ? "-"
                                          : options.signals_output_path.string(),
      options.lag_vol_guard_audit_output_path.empty()
          ? "-"
          : options.lag_vol_guard_audit_output_path.string()
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
          ,
      options.diagnostic_mode.empty() ? "-" : options.diagnostic_mode,
      options.market_calc_output_dir.empty()
          ? "-"
          : options.market_calc_output_dir.string()
#endif
          ,
      loaded.strategy_config.name, ModeText(loaded.strategy_config.mode),
      loaded.strategy_config.strategy_id, loaded.strategy_config.order_capacity,
      loaded.data_reader_config.name, loaded.data_reader_config.sources.size(),
      CountInputFiles(loaded.data_reader_config),
      loaded.lead_lag_config.pairs.size());
  NOVA_INFO(
      "lead_lag_replay config={} data_reader_config={} signals_output={} "
      "lag_vol_guard_audit_output={}"
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      " diagnostic_mode={} market_calc_output_dir={}"
#endif
      " strategy_name={} mode={} strategy_id={} order_capacity={} "
      "reader_name={} sources={} files={} pairs={}",
      options.config_path.string(),
      loaded.strategy_config.data_reader.config_path.string(),
      options.signals_output_path.empty() ? "-"
                                          : options.signals_output_path.string(),
      options.lag_vol_guard_audit_output_path.empty()
          ? "-"
          : options.lag_vol_guard_audit_output_path.string()
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
          ,
      options.diagnostic_mode.empty() ? "-" : options.diagnostic_mode,
      options.market_calc_output_dir.empty()
          ? "-"
          : options.market_calc_output_dir.string()
#endif
          ,
      loaded.strategy_config.name, ModeText(loaded.strategy_config.mode),
      loaded.strategy_config.strategy_id, loaded.strategy_config.order_capacity,
      loaded.data_reader_config.name, loaded.data_reader_config.sources.size(),
      CountInputFiles(loaded.data_reader_config),
      loaded.lead_lag_config.pairs.size());
}

int RunReplay(LoadedConfig loaded, const CliOptions& options) {
  using HistoricalReader = market_data::HistoricalDataReader<
      market_data::HistoricalDataReaderDiagnostics>;
  using Runtime =
      core::TradingRuntime<ReplayStrategy, NullOrderSession, HistoricalReader>;

#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  const bool market_calc_mode = MarketCalcDiagnosticMode(options);
  if (!options.diagnostic_mode.empty() && !market_calc_mode) {
    fmt::print(stderr, "[FAIL] unsupported diagnostic_mode={}\n",
               options.diagnostic_mode);
    NOVA_ERROR("unsupported diagnostic_mode={}", options.diagnostic_mode);
    return 1;
  }
  if (market_calc_mode && options.market_calc_output_dir.empty()) {
    fmt::print(stderr,
               "[FAIL] --market-calc-output-dir is required for "
               "--diagnostic-mode market_calc\n");
    NOVA_ERROR("--market-calc-output-dir is required for "
               "--diagnostic-mode market_calc");
    return 1;
  }
  if (market_calc_mode && !options.signals_output_path.empty()) {
    fmt::print(stderr,
               "[FAIL] --signals-output is not used in market_calc "
               "diagnostic mode\n");
    NOVA_ERROR("--signals-output is not used in market_calc diagnostic mode");
    return 1;
  }
#endif

  ReplayStats stats;
  leadlag::SignalCsvWriter signal_writer;
  leadlag::SignalCsvWriter* signal_writer_ptr = nullptr;
  if (!options.signals_output_path.empty()) {
    std::string error;
    if (!signal_writer.Open(options.signals_output_path, &error)) {
      fmt::print(stderr, "[FAIL] signals_output_error={}\n", error);
      NOVA_ERROR("signals_output_error={}", error);
      return 1;
    }
    signal_writer_ptr = &signal_writer;
  }
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  leadlag::MarketCalcCsvWriter market_calc_writer;
  leadlag::MarketCalcCsvWriter* market_calc_writer_ptr = nullptr;
  if (market_calc_mode) {
    std::string error;
    if (!market_calc_writer.Open(options.market_calc_output_dir, &error)) {
      fmt::print(stderr, "[FAIL] market_calc_output_error={}\n", error);
      NOVA_ERROR("market_calc_output_error={}", error);
      return 1;
    }
    market_calc_writer_ptr = &market_calc_writer;
  }
#endif

  leadlag::Config lead_lag_config = std::move(loaded.lead_lag_config);
  leadlag_tools::LagVolGuardAuditCsvWriter lag_vol_audit_writer;
  leadlag_tools::LagVolGuardAuditCsvWriter* lag_vol_audit_writer_ptr =
      nullptr;
  std::unique_ptr<leadlag_tools::LagVolGuardAuditCollector>
      lag_vol_audit_collector;
  if (!options.lag_vol_guard_audit_output_path.empty()) {
    leadlag_tools::LagVolGuardAuditConfig audit_config;
    if (!BuildLagVolGuardAuditConfig(options, &audit_config)) {
      return 1;
    }
    std::string error;
    if (!lag_vol_audit_writer.Open(options.lag_vol_guard_audit_output_path,
                                   &error)) {
      fmt::print(stderr, "[FAIL] lag_vol_guard_audit_error={}\n", error);
      NOVA_ERROR("lag_vol_guard_audit_error={}", error);
      return 1;
    }
    lag_vol_audit_writer_ptr = &lag_vol_audit_writer;
    lag_vol_audit_collector =
        std::make_unique<leadlag_tools::LagVolGuardAuditCollector>(
            leadlag_tools::BuildLagVolGuardAuditPairs(lead_lag_config),
            audit_config);
  }

  leadlag::StrategyOptions strategy_options{
      .position_accounting = leadlag::PositionAccountingMode::kSyntheticSignals,
  };
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  strategy_options.market_calc_diagnostics_only = market_calc_mode;
#endif
  auto runtime_result = Runtime::Create(
      std::move(loaded.strategy_config), std::move(loaded.data_reader_config),
      [] { return NullOrderSession{}; }, std::move(lead_lag_config),
      strategy_options, &stats, signal_writer_ptr,
      lag_vol_audit_collector.get(), lag_vol_audit_writer_ptr
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      ,
      market_calc_writer_ptr
#endif
  );
  if (!runtime_result.ok) {
    fmt::print(stderr, "[FAIL] runtime_create_error={}\n",
               runtime_result.error);
    NOVA_ERROR("runtime_create_error={}", runtime_result.error);
    return 1;
  }

  const int exit_code = runtime_result.value->Run();
  signal_writer.Close();
  lag_vol_audit_writer.Close();
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  market_calc_writer.Close();
#endif
  fmt::print(
      "lead_lag_replay_summary exit_code={} book_tickers={} signals={} "
      "open={} close={} stoploss={} signals_output={} "
      "lag_vol_guard_audit_output={}"
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      " diagnostic_mode={} market_calc_output_dir={}"
#endif
      "\n",
      exit_code, stats.book_tickers, stats.signals, stats.open_signals,
      stats.close_signals, stats.stoploss_signals,
      options.signals_output_path.empty() ? "-"
                                          : options.signals_output_path.string(),
      options.lag_vol_guard_audit_output_path.empty()
          ? "-"
          : options.lag_vol_guard_audit_output_path.string()
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
          ,
      options.diagnostic_mode.empty() ? "-" : options.diagnostic_mode,
      options.market_calc_output_dir.empty()
          ? "-"
          : options.market_calc_output_dir.string()
#endif
  );
  NOVA_INFO(
      "lead_lag_replay_summary exit_code={} book_tickers={} signals={} "
      "open={} close={} stoploss={} signals_output={} "
      "lag_vol_guard_audit_output={}"
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      " diagnostic_mode={} market_calc_output_dir={}"
#endif
      ,
      exit_code, stats.book_tickers, stats.signals, stats.open_signals,
      stats.close_signals, stats.stoploss_signals,
      options.signals_output_path.empty() ? "-"
                                          : options.signals_output_path.string(),
      options.lag_vol_guard_audit_output_path.empty()
          ? "-"
          : options.lag_vol_guard_audit_output_path.string()
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
          ,
      options.diagnostic_mode.empty() ? "-" : options.diagnostic_mode,
      options.market_calc_output_dir.empty()
          ? "-"
          : options.market_calc_output_dir.string()
#endif
  );
  return exit_code;
}

int Run(const CliOptions& options) {
  if (!options.lag_vol_guard_audit_output_path.empty()) {
    leadlag_tools::LagVolGuardAuditConfig audit_config;
    if (!BuildLagVolGuardAuditConfig(options, &audit_config)) {
      return 1;
    }
  }

  LoadedConfig loaded;
  if (!LoadConfig(options, &loaded)) {
    return 1;
  }
  PrintLoadedConfigSummary(loaded, options);
  return RunReplay(std::move(loaded), options);
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;

  CLI::App app{
      "Replay lead_lag strategy signals from Aquila BookTicker binary files"};
  app.add_option("--config", options.config_path, "Trading runtime TOML path");
  app.add_option("--data-reader-config", options.data_reader_config_path,
                 "Override strategy.data_reader.config path");
  app.add_option("--signals-output", options.signals_output_path,
                 "Optional CSV path for triggered replay signals");
  app.add_option("--lag-vol-guard-audit-output",
                 options.lag_vol_guard_audit_output_path,
                 "Optional CSV path for replay-only lag vol guard audit");
  app.add_option("--lag-vol-guard-jump-threshold",
                 options.lag_vol_guard_jump_threshold,
                 "Lag vol guard jump threshold ratio");
  app.add_option("--lag-vol-guard-jump-count",
                 options.lag_vol_guard_jump_count,
                 "Lag vol guard jump count threshold");
  app.add_option("--lag-vol-guard-jump-window",
                 options.lag_vol_guard_jump_window,
                 "Lag vol guard jump window duration");
  app.add_option("--lag-vol-guard-amplitude-threshold",
                 options.lag_vol_guard_amplitude_threshold,
                 "Lag vol guard amplitude threshold ratio");
  app.add_option("--lag-vol-guard-amplitude-window",
                 options.lag_vol_guard_amplitude_window,
                 "Lag vol guard amplitude window duration");
  app.add_option("--lag-vol-guard-cooldown",
                 options.lag_vol_guard_cooldown,
                 "Lag vol guard cooldown duration");
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  app.add_option("--diagnostic-mode", options.diagnostic_mode,
                 "Optional diagnostic mode; supported value: market_calc");
  app.add_option("--market-calc-output-dir", options.market_calc_output_dir,
                 "Output directory for market_calc lead_calc.csv and "
                 "lag_calc.csv");
#endif
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::parse_result toml =
        toml::parse_file(options.config_path.string());
    nova::LoggingGuard logging_guard{toml};
    return Run(options);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "[FAIL] replay_error={}\n", exc.what());
    return 1;
  }
}
