#include <cstdint>
#include <exception>
#include <filesystem>
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

namespace {

namespace config = aquila::config;
namespace leadlag = aquila::strategy::leadlag;
namespace market_data = aquila::market_data;
namespace core = aquila::core;

struct CliOptions {
  std::filesystem::path config_path{
      "config/strategies/lead_lag_ordi_replay.toml"};
  std::filesystem::path data_reader_config_path;
  std::filesystem::path signals_output_path;
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
                 ReplayStats* stats, leadlag::SignalCsvWriter* signal_writer
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
                 ,
                 leadlag::MarketCalcCsvWriter* market_calc_writer
#endif
                 )
      : inner_(std::move(config), options),
        stats_(stats),
        signal_writer_(signal_writer) {
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
    if (market_calc_writer != nullptr) {
      inner_.SetMarketCalcObserver(market_calc_writer, WriteMarketCalcRow);
    }
#endif
  }

  ReplayStrategy(const ReplayStrategy&) = delete;
  ReplayStrategy& operator=(const ReplayStrategy&) = delete;
  ReplayStrategy(ReplayStrategy&&) noexcept = default;
  ReplayStrategy& operator=(ReplayStrategy&&) noexcept = default;

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
    return false;
  }
  if (loaded.strategy_config.feedback.enabled) {
    fmt::print(stderr,
               "[FAIL] strategy.feedback.enabled must be false for replay\n");
    return false;
  }
  if (loaded.lead_lag_config.pairs.empty()) {
    fmt::print(stderr,
               "[FAIL] lead_lag config must contain at least one pair\n");
    return false;
  }
  return true;
}

bool LoadConfig(const CliOptions& options, LoadedConfig* loaded) {
  auto strategy_result = config::LoadStrategyConfigFile(options.config_path);
  if (!strategy_result.ok) {
    fmt::print(stderr, "[FAIL] strategy_config_error={}\n",
               strategy_result.error);
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
    return false;
  }
  loaded->data_reader_config = std::move(data_reader_result.value);

  auto lead_lag_result =
      leadlag::LoadConfigFile(loaded->strategy_config.user_config_path,
                              loaded->data_reader_config.instrument_catalog);
  if (!lead_lag_result.ok) {
    fmt::print(stderr, "[FAIL] lead_lag_config_error={}\n",
               lead_lag_result.error);
    return false;
  }
  loaded->lead_lag_config = std::move(lead_lag_result.value);

  return ValidateLoadedConfig(*loaded);
}

void PrintLoadedConfigSummary(const LoadedConfig& loaded,
                              const CliOptions& options) {
  fmt::print(
      "lead_lag_replay config={} data_reader_config={} "
      "signals_output={}"
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      " diagnostic_mode={} market_calc_output_dir={}"
#endif
      " strategy_name={} mode={} strategy_id={} "
      "order_capacity={} reader_name={} sources={} files={} pairs={}\n",
      options.config_path.string(),
      loaded.strategy_config.data_reader.config_path.string(),
      options.signals_output_path.empty() ? "-"
                                          : options.signals_output_path.string()
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
    return 1;
  }
  if (market_calc_mode && options.market_calc_output_dir.empty()) {
    fmt::print(stderr,
               "[FAIL] --market-calc-output-dir is required for "
               "--diagnostic-mode market_calc\n");
    return 1;
  }
  if (market_calc_mode && !options.signals_output_path.empty()) {
    fmt::print(stderr,
               "[FAIL] --signals-output is not used in market_calc "
               "diagnostic mode\n");
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
      return 1;
    }
    market_calc_writer_ptr = &market_calc_writer;
  }
#endif

  leadlag::Config lead_lag_config = std::move(loaded.lead_lag_config);
  leadlag::StrategyOptions strategy_options{
      .position_accounting = leadlag::PositionAccountingMode::kSyntheticSignals,
  };
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  strategy_options.market_calc_diagnostics_only = market_calc_mode;
#endif
  auto runtime_result = Runtime::Create(
      std::move(loaded.strategy_config), std::move(loaded.data_reader_config),
      [] { return NullOrderSession{}; }, std::move(lead_lag_config),
      strategy_options, &stats, signal_writer_ptr
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      ,
      market_calc_writer_ptr
#endif
  );
  if (!runtime_result.ok) {
    fmt::print(stderr, "[FAIL] runtime_create_error={}\n",
               runtime_result.error);
    return 1;
  }

  const int exit_code = runtime_result.value->Run();
  signal_writer.Close();
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  market_calc_writer.Close();
#endif
  fmt::print(
      "lead_lag_replay_summary exit_code={} book_tickers={} signals={} "
      "open={} close={} stoploss={} signals_output={}"
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      " diagnostic_mode={} market_calc_output_dir={}"
#endif
      "\n",
      exit_code, stats.book_tickers, stats.signals, stats.open_signals,
      stats.close_signals, stats.stoploss_signals,
      options.signals_output_path.empty() ? "-"
                                          : options.signals_output_path.string()
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
