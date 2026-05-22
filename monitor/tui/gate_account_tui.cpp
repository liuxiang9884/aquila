#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

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
  int width{220};
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
    RefreshMarketDataRows();
  }

  void MarkLiveMarketDataEnabled() {
    SetMode("read-only live market data");
    SetSingleEvent("live market data enabled");
  }

  void MarkLiveMarketDataDump() {
    SetMode("read-only live market data dump");
    SetSingleEvent("live market data dump snapshot");
  }

  void MarkMarketDataUnavailable() {
    SetMode("market data unavailable");
    SetSingleEvent("market data unavailable");
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
    event_text_ = event;
    snapshot_.events = std::span<const std::string_view>{&event_text_, 1};
  }

  aquila::monitor::AccountMonitorSnapshot snapshot_;
  aquila::monitor::SymbolDetail selected_detail_{};
  std::optional<aquila::monitor::MarketDataViewModel> market_data_model_;
  std::string mode_text_;
  std::string_view event_text_;
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
    state.MarkMarketDataUnavailable();
    return false;
  }
  state.EnableMarketDataRows(
      config_result.value.instrument_catalog.instruments());
  return true;
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
  CLI::App app{"Aquila Gate account TUI demo"};
  app.add_flag("--dump", options.dump,
               "Render one static frame and exit instead of opening the TUI");
  app.add_flag("--live-market-data", options.live_market_data,
               "Attach live market data rows from monitor SHM");
  app.add_option("--market-data-config", options.market_data_config,
                 "Market data reader config for live TUI rows");
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
      state.MarkLiveMarketDataDump();
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
      market_data_queue_for_ui = &market_data_queue;
      market_data_thread = std::move(thread);
    } else {
      state.MarkMarketDataUnavailable();
    }
  }

  const int result = RunInteractive(state, view, market_data_queue_for_ui);
  if (market_data_thread != nullptr) {
    market_data_thread->Stop();
    market_data_thread->Join();
  }
  return result;
}
