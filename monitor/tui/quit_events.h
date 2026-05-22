#ifndef AQUILA_MONITOR_TUI_QUIT_EVENTS_H_
#define AQUILA_MONITOR_TUI_QUIT_EVENTS_H_

#include <ftxui/component/event.hpp>

namespace aquila::monitor {

[[nodiscard]] inline bool IsQuitEvent(const ftxui::Event& event) noexcept {
  return event == ftxui::Event::q || event == ftxui::Event::Q ||
         event == ftxui::Event::CtrlC || event == ftxui::Event::CtrlQ ||
         event == ftxui::Event::Escape || event == ftxui::Event::Character('q');
}

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_TUI_QUIT_EVENTS_H_
