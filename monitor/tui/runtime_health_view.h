#ifndef AQUILA_MONITOR_TUI_RUNTIME_HEALTH_VIEW_H_
#define AQUILA_MONITOR_TUI_RUNTIME_HEALTH_VIEW_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/core.h>
#include <ftxui/dom/elements.hpp>

#include "monitor/model/account_monitor_snapshot.h"

namespace aquila::monitor {

namespace runtime_health_view_detail {

inline ftxui::Color StateColor(std::string_view state) {
  if (state == "ok" || state == "up" || state == "connected") {
    return ftxui::Color::Green;
  }
  if (state == "warn" || state == "stale" || state == "restarting") {
    return ftxui::Color::Yellow;
  }
  if (state == "down") {
    return ftxui::Color::Red;
  }
  return ftxui::Color::GrayLight;
}

inline ftxui::Element Title(std::string text) {
  return ftxui::text(std::move(text)) | ftxui::color(ftxui::Color::GrayLight);
}

inline ftxui::Element ServerMetricRow(const ServerMetric& metric) {
  return ftxui::text(fmt::format("{:<14} {:<14} {:<8} {:<12}", metric.metric,
                                 metric.value, metric.state,
                                 metric.updated_time)) |
         ftxui::color(StateColor(metric.state));
}

inline ftxui::Element ProcessRow(std::size_t visible_index,
                                 const ProcessHealth& process) {
  return ftxui::text(fmt::format(
             "{:>3} {:<28} {:<9} {:>7} {:<10} {:>5.1f} {:>5.1f} {:<10} "
             "{:<12} {:<12}",
             visible_index, process.name, process.role, process.pid,
             process.uptime, process.cpu_percent, process.memory_percent,
             process.status, process.heartbeat, process.updated_time)) |
         ftxui::color(StateColor(process.status));
}

inline ftxui::Element ConnectionRow(std::size_t visible_index,
                                    const ConnectionHealth& connection) {
  return ftxui::text(fmt::format(
             "{:>3} {:<10} {:<18} {:<11} {:<8} {:<12} {:>10} {:<12}",
             visible_index, connection.venue, connection.channel,
             connection.state, connection.latency, connection.last_message,
             connection.reconnects, connection.updated_time)) |
         ftxui::color(StateColor(connection.state));
}

inline ftxui::Element ServerPane(const RuntimeHealth& health) {
  ftxui::Elements rows;
  rows.push_back(Title("SERVER"));
  rows.push_back(ftxui::separator());
  rows.push_back(ftxui::text("metric         value          state    updated") |
                 ftxui::color(ftxui::Color::GrayLight));
  for (const ServerMetric& metric : health.server_metrics) {
    rows.push_back(ServerMetricRow(metric));
  }
  return ftxui::vbox(std::move(rows)) | ftxui::border;
}

inline ftxui::Element ProcessesPane(const RuntimeHealth& health) {
  ftxui::Elements rows;
  rows.push_back(Title("PROCESSES"));
  rows.push_back(ftxui::separator());
  rows.push_back(
      ftxui::text("num name                         role          pid uptime"
                  "       cpu   mem status     heartbeat    updated") |
      ftxui::color(ftxui::Color::GrayLight));
  for (std::size_t i = 0; i < health.processes.size(); ++i) {
    rows.push_back(ProcessRow(i + 1, health.processes[i]));
  }
  return ftxui::vbox(std::move(rows)) | ftxui::border;
}

inline ftxui::Element ConnectionsPane(const RuntimeHealth& health) {
  ftxui::Elements rows;
  rows.push_back(Title("CONNECTIONS"));
  rows.push_back(ftxui::separator());
  rows.push_back(
      ftxui::text("num venue      channel            state       latency "
                  "last_msg     reconnects updated") |
      ftxui::color(ftxui::Color::GrayLight));
  for (std::size_t i = 0; i < health.connections.size(); ++i) {
    rows.push_back(ConnectionRow(i + 1, health.connections[i]));
  }
  return ftxui::vbox(std::move(rows)) | ftxui::border;
}

}  // namespace runtime_health_view_detail

inline ftxui::Element RenderHealthSummaryStrip(const RuntimeHealth& health) {
  return ftxui::hbox({
             runtime_health_view_detail::Title("HEALTH"),
             ftxui::text(fmt::format("  server {}", health.server_state)) |
                 ftxui::color(runtime_health_view_detail::StateColor(
                     health.server_state)),
             ftxui::text(fmt::format(" | cpu {:.0f}%", health.cpu_percent)),
             ftxui::text(fmt::format(" | mem {:.0f}%", health.memory_percent)),
             ftxui::text(fmt::format(" | disk {:.0f}%", health.disk_percent)),
             ftxui::text(fmt::format(" | md {}/{} up",
                                     health.market_processes_up,
                                     health.market_processes_total)),
             ftxui::text(fmt::format(" | trading {}/{} up",
                                     health.trading_processes_up,
                                     health.trading_processes_total)),
             ftxui::text(fmt::format(" | stale {}", health.stale_count)) |
                 ftxui::color(runtime_health_view_detail::StateColor(
                     health.stale_count == 0 ? "ok" : "warn")),
             ftxui::text(fmt::format(" | restart {}", health.restart_count)),
         }) |
         ftxui::border;
}

inline ftxui::Element RenderRuntimeHealthPage(
    const AccountMonitorSnapshot& snapshot) {
  const RuntimeHealth& health = snapshot.runtime_health;
  return ftxui::vbox({
             ftxui::hbox({
                 ftxui::text("AQUILA RUNTIME HEALTH") | ftxui::bold,
                 ftxui::filler(),
                 ftxui::text(std::string(snapshot.mode)) |
                     ftxui::color(ftxui::Color::Yellow),
                 ftxui::text("  q/esc quit | --view workbench"),
             }),
             RenderHealthSummaryStrip(health),
             ftxui::separator(),
             runtime_health_view_detail::ServerPane(health),
             runtime_health_view_detail::ProcessesPane(health),
             runtime_health_view_detail::ConnectionsPane(health),
         }) |
         ftxui::border;
}

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_TUI_RUNTIME_HEALTH_VIEW_H_
