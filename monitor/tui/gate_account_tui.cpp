#include <cstdlib>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "monitor/demo/symbol_workbench_demo_data.h"
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
  int width{220};
  int height{55};
  std::string view{"workbench"};
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

int RunInteractive(const aquila::monitor::AccountMonitorSnapshot& snapshot,
                   ViewKind view) {
  auto screen = ftxui::ScreenInteractive::Fullscreen();
  auto component = ftxui::Renderer([&] { return RenderView(snapshot, view); });
  component = ftxui::CatchEvent(component, [&](ftxui::Event event) {
    if (aquila::monitor::IsQuitEvent(event)) {
      screen.ExitLoopClosure()();
      return true;
    }
    return false;
  });
  screen.Loop(component);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;
  CLI::App app{"Aquila Gate account TUI demo"};
  app.add_flag("--dump", options.dump,
               "Render one static frame and exit instead of opening the TUI");
  app.add_option("--width", options.width, "Dump width in terminal columns");
  app.add_option("--height", options.height, "Dump height in terminal rows");
  app.add_option("--view", options.view, "Initial view: workbench or health")
      ->check(CLI::IsMember({"workbench", "health"}));
  CLI11_PARSE(app, argc, argv);

  const aquila::monitor::AccountMonitorSnapshot snapshot =
      aquila::monitor::DemoAccountMonitorSnapshot();
  const ViewKind view = ParseViewKind(options.view);
  if (options.dump) {
    return Dump(snapshot, view, options.width, options.height);
  }
  return RunInteractive(snapshot, view);
}
