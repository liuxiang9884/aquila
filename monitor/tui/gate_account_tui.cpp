#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "core/config/data_reader_config.h"
#include "monitor/demo/symbol_workbench_demo_data.h"
#include "monitor/market_data/market_data_thread.h"
#include "monitor/model/market_data_view_model.h"
#include "monitor/tui/quit_events.h"
#include "monitor/tui/runtime_health_view.h"
#include "monitor/tui/symbol_workbench_view.h"

namespace {

enum class ViewKind {
  kWorkbench,
  kHealth,
};

struct CliOptions {
  bool dump{false};
  bool live_market_data{false};
  int width{260};
  int height{55};
  std::string view{"workbench"};
  std::filesystem::path market_data_config{
      "config/monitors/gate_account_tui_market_data.toml"};
};

class TuiSnapshotState {
 public:
  explicit TuiSnapshotState(aquila::monitor::AccountMonitorSnapshot snapshot)
      : snapshot_(snapshot) {
    if (snapshot_.selected_detail != nullptr) {
      selected_detail_ = *snapshot_.selected_detail;
      snapshot_.selected_detail = &selected_detail_;
    }
    base_alerts_.assign(snapshot_.runtime_health.alerts.begin(),
                        snapshot_.runtime_health.alerts.end());
    RebuildRuntimeAlerts();
  }

  void EnableMarketDataRows(
      std::span<const aquila::config::InstrumentInfo> instruments) {
    market_data_model_.emplace(snapshot_.symbols, instruments,
                               snapshot_.selected_symbol);
    RefreshMarketDataRows();
  }

  void ApplyMarketDataBatch(const aquila::monitor::MarketDataBatch& batch) {
    if (!market_data_model_.has_value()) {
      return;
    }
    market_data_model_->ApplyBatch(batch);
    RecordMarketDataDiagnostics(batch);
    RefreshMarketDataRows();
  }

  void AddMarketDataSourceAlerts(
      std::span<const aquila::monitor::MarketDataUnavailableSource> sources) {
    for (const aquila::monitor::MarketDataUnavailableSource& source : sources) {
      AddRuntimeAlert(
          source.required ? "error" : "warning",
          fmt::format("market data shm {} {} unavailable: {}",
                      ExchangeText(source.exchange), source.name, source.reason));
    }
  }

  void MarkLiveMarketDataEnabled() {
    SetMode("read-only live market data");
    SetSingleEvent("live market data enabled");
  }

  void MarkLiveMarketDataDump() {
    SetMode("read-only live market data snapshot");
    SetSingleEvent("live market data snapshot");
  }

  void MarkMarketDataUnavailable(std::string_view reason) {
    SetMode("market data unavailable");
    const std::string message =
        reason.empty() ? std::string{"market data unavailable"}
                       : fmt::format("market data unavailable: {}", reason);
    SetSingleEvent(message);
    AddRuntimeAlert("error", message);
  }

  [[nodiscard]] const aquila::monitor::AccountMonitorSnapshot& snapshot()
      const noexcept {
    return snapshot_;
  }

 private:
  void RefreshMarketDataRows() {
    if (market_data_model_.has_value()) {
      selected_detail_.market_data = market_data_model_->SelectedRows();
      snapshot_.selected_detail = &selected_detail_;
    }
  }

  void SetMode(std::string mode) {
    mode_text_ = std::move(mode);
    snapshot_.mode = mode_text_;
  }

  void SetSingleEvent(std::string_view event) {
    event_text_storage_.assign(event);
    event_text_ = event_text_storage_;
    snapshot_.events = std::span<const std::string_view>{&event_text_, 1};
  }

  static std::string ExchangeText(aquila::Exchange exchange) {
    switch (exchange) {
      case aquila::Exchange::kGate:
        return "Gate";
      case aquila::Exchange::kBinance:
        return "Binance";
      case aquila::Exchange::kOkx:
        return "OKX";
      case aquila::Exchange::kBybit:
        return "Bybit";
      case aquila::Exchange::kBitget:
        return "Bitget";
      case aquila::Exchange::kCoinbase:
        return "Coinbase";
    }
    return "Unknown";
  }

  struct OwnedRuntimeAlert {
    std::string severity;
    std::string message;
    std::string updated_time;
  };

  void AddRuntimeAlert(std::string severity, std::string message) {
    owned_alerts_.push_back(OwnedRuntimeAlert{
        .severity = std::move(severity),
        .message = std::move(message),
        .updated_time = "-",
    });
    RebuildRuntimeAlerts();
  }

  void RebuildRuntimeAlerts() {
    alert_views_ = base_alerts_;
    alert_views_.reserve(base_alerts_.size() + owned_alerts_.size());
    for (const OwnedRuntimeAlert& alert : owned_alerts_) {
      alert_views_.push_back(aquila::monitor::RuntimeAlert{
          .severity = alert.severity,
          .message = alert.message,
          .updated_time = alert.updated_time,
      });
    }
    snapshot_.runtime_health.alerts = alert_views_;
  }

  void RecordMarketDataDiagnostics(
      const aquila::monitor::MarketDataBatch& batch) {
    if (batch.overrun_count > last_market_data_overrun_count_) {
      last_market_data_overrun_count_ = batch.overrun_count;
      AddRuntimeAlert(
          "warning",
          fmt::format("market data shm overrun count {}", batch.overrun_count));
    }
    if (batch.dropped_batch_count > last_market_data_dropped_batch_count_) {
      last_market_data_dropped_batch_count_ = batch.dropped_batch_count;
      AddRuntimeAlert("warning",
                      fmt::format("market data UI dropped batch count {}",
                                  batch.dropped_batch_count));
    }
  }

  aquila::monitor::AccountMonitorSnapshot snapshot_;
  aquila::monitor::SymbolDetail selected_detail_{};
  std::optional<aquila::monitor::MarketDataViewModel> market_data_model_;
  std::string mode_text_;
  std::string event_text_storage_;
  std::string_view event_text_;
  std::vector<aquila::monitor::RuntimeAlert> base_alerts_;
  std::vector<aquila::monitor::RuntimeAlert> alert_views_;
  std::vector<OwnedRuntimeAlert> owned_alerts_;
  std::uint64_t last_market_data_overrun_count_{0};
  std::uint64_t last_market_data_dropped_batch_count_{0};
};

ViewKind ParseViewKind(std::string_view view) {
  if (view == "health") {
    return ViewKind::kHealth;
  }
  return ViewKind::kWorkbench;
}

ftxui::Element RenderView(
    const aquila::monitor::AccountMonitorSnapshot& snapshot, ViewKind view) {
  if (view == ViewKind::kHealth) {
    return aquila::monitor::RenderRuntimeHealthPage(snapshot);
  }
  return aquila::monitor::RenderSymbolWorkbench(snapshot);
}

int Dump(const aquila::monitor::AccountMonitorSnapshot& snapshot, ViewKind view,
         int width, int height) {
  ftxui::Element document = RenderView(snapshot, view);
  ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                               ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, document);
  fmt::print("{}", screen.ToString());
  return 0;
}

void DrainMarketDataQueue(aquila::monitor::MarketDataThreadQueue& queue,
                          TuiSnapshotState& state) {
  aquila::monitor::MarketDataBatch batch{};
  while (queue.TryPop(&batch)) {
    state.ApplyMarketDataBatch(batch);
  }
}

bool LoadMarketDataModel(TuiSnapshotState& state,
                         const std::filesystem::path& config_path) {
  const aquila::config::DataReaderConfigResult config_result =
      aquila::config::LoadDataReaderConfigFile(config_path);
  if (!config_result.ok) {
    state.MarkMarketDataUnavailable(config_result.error);
    return false;
  }
  state.EnableMarketDataRows(
      config_result.value.instrument_catalog.instruments());
  return true;
}

void StopAndDrainMarketDataThread(
    aquila::monitor::MarketDataThread* thread,
    aquila::monitor::MarketDataThreadQueue* queue, TuiSnapshotState* state) {
  thread->Stop();
  thread->Join();
  DrainMarketDataQueue(*queue, *state);
}

void RunBoundedLiveMarketDataSnapshot(
    TuiSnapshotState& state, const std::filesystem::path& config_path) {
  aquila::monitor::MarketDataThreadQueue market_data_queue;
  aquila::monitor::MarketDataThread thread(config_path, market_data_queue);
  if (!thread.Start()) {
    state.MarkMarketDataUnavailable(thread.last_error());
    state.AddMarketDataSourceAlerts(thread.unavailable_sources());
    return;
  }

  state.AddMarketDataSourceAlerts(thread.unavailable_sources());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  StopAndDrainMarketDataThread(&thread, &market_data_queue, &state);
  state.MarkLiveMarketDataDump();
}

int RunInteractive(TuiSnapshotState& state, ViewKind view,
                   aquila::monitor::MarketDataThreadQueue* market_data_queue) {
  auto screen = ftxui::ScreenInteractive::Fullscreen();
  auto component =
      ftxui::Renderer([&] { return RenderView(state.snapshot(), view); });
  component = ftxui::CatchEvent(component, [&](ftxui::Event event) {
    if (aquila::monitor::IsQuitEvent(event)) {
      screen.ExitLoopClosure()();
      return true;
    }
    return false;
  });
  ftxui::Loop loop(&screen, component);
  while (!loop.HasQuitted()) {
    if (market_data_queue != nullptr) {
      DrainMarketDataQueue(*market_data_queue, state);
    }
    loop.RunOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;
  CLI::App app{"Aquila Gate account TUI monitor"};
  app.add_flag("--dump", options.dump,
               "Render one static frame and exit instead of opening the TUI");
  app.add_flag("--live-market-data", options.live_market_data,
               "Attach live market data rows from monitor SHM");
  app.add_option("--market-data-config", options.market_data_config,
                 "Market data reader config used with --live-market-data");
  app.add_option("--width", options.width, "Dump width in terminal columns");
  app.add_option("--height", options.height, "Dump height in terminal rows");
  app.add_option("--view", options.view, "Initial view: workbench or health")
      ->check(CLI::IsMember({"workbench", "health"}));
  CLI11_PARSE(app, argc, argv);

  const ViewKind view = ParseViewKind(options.view);
  if (options.dump) {
    if (!options.live_market_data) {
      return Dump(aquila::monitor::DemoAccountMonitorSnapshot(), view,
                  options.width, options.height);
    }
    TuiSnapshotState state{aquila::monitor::DemoAccountMonitorSnapshot()};
    if (LoadMarketDataModel(state, options.market_data_config)) {
      RunBoundedLiveMarketDataSnapshot(state, options.market_data_config);
    }
    return Dump(state.snapshot(), view, options.width, options.height);
  }

  TuiSnapshotState state{aquila::monitor::DemoAccountMonitorSnapshot()};
  aquila::monitor::MarketDataThreadQueue market_data_queue;
  aquila::monitor::MarketDataThreadQueue* market_data_queue_for_ui = nullptr;
  std::unique_ptr<aquila::monitor::MarketDataThread> market_data_thread;
  if (options.live_market_data &&
      LoadMarketDataModel(state, options.market_data_config)) {
    auto thread = std::make_unique<aquila::monitor::MarketDataThread>(
        options.market_data_config, market_data_queue);
    if (thread->Start()) {
      state.MarkLiveMarketDataEnabled();
      state.AddMarketDataSourceAlerts(thread->unavailable_sources());
      market_data_queue_for_ui = &market_data_queue;
      market_data_thread = std::move(thread);
    } else {
      state.MarkMarketDataUnavailable(thread->last_error());
      state.AddMarketDataSourceAlerts(thread->unavailable_sources());
    }
  }

  const int result = RunInteractive(state, view, market_data_queue_for_ui);
  if (market_data_thread != nullptr) {
    market_data_thread->Stop();
    market_data_thread->Join();
  }
  return result;
}
