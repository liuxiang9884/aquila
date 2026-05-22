#ifndef AQUILA_MONITOR_TUI_SYMBOL_WORKBENCH_VIEW_H_
#define AQUILA_MONITOR_TUI_SYMBOL_WORKBENCH_VIEW_H_

#include <string>
#include <string_view>
#include <vector>

#include <fmt/core.h>
#include <ftxui/dom/elements.hpp>

#include "monitor/model/account_monitor_snapshot.h"

namespace aquila::monitor {

namespace symbol_workbench_view_detail {

inline std::string SideText(OrderSide side) {
  return side == OrderSide::kBuy ? "buy" : "sell";
}

inline ftxui::Color PnlColor(double value) {
  if (value > 0.0) {
    return ftxui::Color::Green;
  }
  if (value < 0.0) {
    return ftxui::Color::Red;
  }
  return ftxui::Color::GrayLight;
}

inline std::string HealthText(SymbolHealth health) {
  switch (health) {
    case SymbolHealth::kOk:
      return "";
    case SymbolHealth::kSelected:
      return "*";
    case SymbolHealth::kDrift:
      return "D";
    case SymbolHealth::kStale:
      return "S";
  }
  return "";
}

inline ftxui::Element BoxTitle(std::string text) {
  return ftxui::text(std::move(text)) | ftxui::color(ftxui::Color::GrayLight);
}

inline ftxui::Element SymbolRow(const SymbolSummary& summary) {
  auto row = ftxui::text(fmt::format("{:<12} {:>7.1f} {:>4} {:>8.1f} {:>1}",
                                     summary.symbol, summary.net_position,
                                     summary.open_order_count,
                                     summary.total_pnl,
                                     HealthText(summary.health)));
  if (summary.health == SymbolHealth::kSelected) {
    return row | ftxui::inverted;
  }
  if (summary.health == SymbolHealth::kDrift) {
    return row | ftxui::color(ftxui::Color::Red);
  }
  if (summary.health == SymbolHealth::kStale) {
    return row | ftxui::color(ftxui::Color::Yellow);
  }
  if (summary.total_pnl != 0.0) {
    return row | ftxui::color(PnlColor(summary.total_pnl));
  }
  return row;
}

inline ftxui::Element OrderRow(const MonitorOrder& order, bool selected) {
  auto row = ftxui::text(fmt::format(
      "{:<10} {:<4} {:>5.1f} {:>5.1f} {:>5.1f} {:>7.2f} {:>7.2f} {:>7.2f} "
      "{:<8}",
      order.source_label, SideText(order.side), order.quantity,
      order.left_quantity, order.filled_quantity, order.price,
      order.average_fill_price, order.fee, order.status));
  if (selected) {
    return row | ftxui::inverted;
  }
  if (order.side == OrderSide::kBuy) {
    return row | ftxui::color(ftxui::Color::Green);
  }
  return row | ftxui::color(ftxui::Color::Red);
}

inline ftxui::Element SymbolPane(const AccountMonitorSnapshot& snapshot) {
  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({
      BoxTitle(fmt::format("SYMBOLS active {} / all {}", snapshot.symbols.size(),
                           snapshot.symbols.size())),
      ftxui::filler(),
      BoxTitle("risk sort"),
  }));
  rows.push_back(ftxui::separator());
  rows.push_back(ftxui::text("symbol           pos open      pnl !") |
                 ftxui::color(ftxui::Color::GrayLight));
  for (const SymbolSummary& summary : snapshot.symbols) {
    rows.push_back(SymbolRow(summary));
  }
  rows.push_back(ftxui::filler());
  rows.push_back(ftxui::separator());
  rows.push_back(ftxui::text("Legend: D drift, S stale, * selected") |
                 ftxui::color(ftxui::Color::GrayLight));
  rows.push_back(ftxui::text("Filter: active only | search: empty") |
                 ftxui::color(ftxui::Color::GrayLight));
  return ftxui::vbox(std::move(rows)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 42);
}

inline ftxui::Element OrdersPane(const SymbolDetail& detail) {
  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({
      BoxTitle(fmt::format("ORDERS: {}", detail.symbol)),
      ftxui::filler(),
      BoxTitle(fmt::format("{} rows / source: all", detail.orders.size())),
  }));
  rows.push_back(ftxui::separator());
  rows.push_back(ftxui::text(
                     "source     side   qty  left  fill      px     avg     fee "
                     "status") |
                 ftxui::color(ftxui::Color::GrayLight));
  for (std::size_t i = 0; i < detail.orders.size(); ++i) {
    rows.push_back(OrderRow(detail.orders[i], i == 1));
  }
  rows.push_back(ftxui::filler());
  rows.push_back(ftxui::separator());
  rows.push_back(BoxTitle("SELECTED ORDER"));
  if (detail.orders.size() > 1) {
    const MonitorOrder& order = detail.orders[1];
    rows.push_back(ftxui::hbox({
        ftxui::vbox({
            ftxui::text(fmt::format("source: {}", order.source_label)),
            ftxui::text(fmt::format("exchange id: {}", order.exchange_order_id)),
            ftxui::text(fmt::format("text: {}", order.text.empty() ? "empty"
                                                                   : order.text)),
            ftxui::text(fmt::format("age: {}s", order.age_seconds)),
        }) | ftxui::flex,
        ftxui::separator(),
        ftxui::vbox({
            ftxui::text(fmt::format("side: {}", SideText(order.side))),
            ftxui::text(fmt::format("qty: {:.1f} left: {:.1f}", order.quantity,
                                    order.left_quantity)),
            ftxui::text(fmt::format("price: {:.2f}", order.price)),
            ftxui::text(fmt::format("status: {}", order.status)),
        }) | ftxui::flex,
    }) | ftxui::border);
  }
  return ftxui::vbox(std::move(rows)) | ftxui::flex;
}

inline ftxui::Element PositionPane(const SymbolDetail& detail) {
  const PositionPnl& pnl = detail.position;
  const SourceMix& mix = detail.source_mix;
  return ftxui::vbox({
             BoxTitle(fmt::format("POSITION / PNL: {}", detail.symbol)),
             ftxui::vbox({
                 ftxui::text(fmt::format("net pos       {:+.1f}", pnl.net_position)) |
                     ftxui::bold | ftxui::color(PnlColor(pnl.net_position)),
                 ftxui::text(fmt::format("open orders   {}", detail.orders.size())),
                 ftxui::text(fmt::format("avg entry     {:.2f}", pnl.average_entry)),
                 ftxui::text(fmt::format("mark          {:.2f}", pnl.mark_price)),
                 ftxui::text(fmt::format("notional      {:.2f}", pnl.notional)),
                 ftxui::text(fmt::format("exposure      {}", pnl.exposure)),
             }) | ftxui::border,
             ftxui::vbox({
                 ftxui::text(fmt::format("realized      {:+.2f}", pnl.realized_pnl)) |
                     ftxui::color(PnlColor(pnl.realized_pnl)),
                 ftxui::text(fmt::format("unrealized    {:+.2f}", pnl.unrealized_pnl)) |
                     ftxui::color(PnlColor(pnl.unrealized_pnl)),
                 ftxui::text(fmt::format("fees          {:+.2f}", pnl.fees)) |
                     ftxui::color(PnlColor(pnl.fees)),
                 ftxui::text(fmt::format("total         {:+.2f}", pnl.total_pnl)) |
                     ftxui::bold | ftxui::color(PnlColor(pnl.total_pnl)),
             }) | ftxui::border,
             BoxTitle("SOURCE MIX"),
             ftxui::vbox({
                 ftxui::text(fmt::format("Aquila        {} orders", mix.aquila)),
                 ftxui::text(fmt::format("Manual        {} orders", mix.manual)),
                 ftxui::text(fmt::format("External      {} orders", mix.external)),
                 ftxui::text(fmt::format("Unknown       {} orders", mix.unknown)),
             }) | ftxui::border,
             BoxTitle("HEALTH"),
             ftxui::vbox({
                 ftxui::text(fmt::format("ws updates    {}s ago",
                                         pnl.ws_update_age_seconds)),
                 ftxui::text(fmt::format("rest snapshot {}s ago",
                                         pnl.rest_snapshot_age_seconds)),
                 ftxui::text(fmt::format("ledger        {}", pnl.ledger_state)),
                 ftxui::text(fmt::format("drift         {}", pnl.drift_state)),
             }) | ftxui::border,
         }) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 42);
}

inline ftxui::Element EventsPane(const AccountMonitorSnapshot& snapshot) {
  ftxui::Elements events;
  events.push_back(BoxTitle("EVENTS"));
  for (std::string_view event : snapshot.events) {
    events.push_back(ftxui::text(std::string(event)));
  }
  return ftxui::vbox(std::move(events)) | ftxui::border;
}

}  // namespace symbol_workbench_view_detail

inline ftxui::Element RenderSymbolWorkbench(
    const AccountMonitorSnapshot& snapshot) {
  const SymbolDetail* detail = snapshot.selected_detail;
  if (detail == nullptr) {
    return ftxui::text("missing selected symbol detail") | ftxui::border;
  }

  return ftxui::vbox({
             ftxui::hbox({
                 ftxui::text(std::string(snapshot.title)) | ftxui::bold,
                 ftxui::filler(),
                 ftxui::text(std::string(snapshot.websocket_state)) |
                     ftxui::color(ftxui::Color::Cyan),
                 ftxui::text("  "),
                 ftxui::text(std::string(snapshot.rest_state)) |
                     ftxui::color(ftxui::Color::Green),
                 ftxui::text("  "),
                 ftxui::text(std::string(snapshot.mode)) |
                     ftxui::color(ftxui::Color::Yellow),
                 ftxui::text("  q quit | / search | r refresh | f active/all") |
                     ftxui::color(ftxui::Color::GrayLight),
             }),
             ftxui::separator(),
             ftxui::hbox({
                 symbol_workbench_view_detail::SymbolPane(snapshot),
                 ftxui::separator(),
                 symbol_workbench_view_detail::OrdersPane(*detail),
                 ftxui::separator(),
                 symbol_workbench_view_detail::PositionPane(*detail),
             }) | ftxui::flex,
             symbol_workbench_view_detail::EventsPane(snapshot),
         }) |
         ftxui::border;
}

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_TUI_SYMBOL_WORKBENCH_VIEW_H_
