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

inline std::string SideValueText(int side_value) {
  return fmt::format("{:+d}", side_value);
}

inline std::string IdText(std::uint64_t id) {
  if (id == 0) {
    return "-";
  }
  return fmt::format("{}", id);
}

inline std::string PriceText(double value) {
  if (value == 0.0) {
    return "NA";
  }
  return fmt::format("{:.2f}", value);
}

inline std::string MarketNumberText(const MarketDataRow& row, double value,
                                    int precision) {
  if (!row.has_data) {
    return "NA";
  }
  if (precision == 1) {
    return fmt::format("{:.1f}", value);
  }
  return fmt::format("{:.2f}", value);
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

inline ftxui::Element SymbolRow(std::size_t visible_index,
                                const SymbolSummary& summary) {
  auto row = ftxui::text(fmt::format(
      "{:>3} {:<12} {:>7.1f} {:>4} {:>8.1f} {:>1}", visible_index,
      summary.symbol, summary.net_position, summary.open_order_count,
      summary.total_pnl, HealthText(summary.health)));
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

inline ftxui::Element OrderRow(std::size_t visible_index,
                               const MonitorOrder& order, bool selected) {
  auto row = ftxui::text(fmt::format(
      "{:>3} {:<7} {:<11} {:<5} {:>8.2f} {:>8.1f} {:>8.1f} {:>8.1f} {:>8} "
      "{:>8.2f} {:<8} {:<10} {:<12} {:<18} {:<12}",
      visible_index, order.exchange, order.exchange_symbol,
      SideValueText(order.side_value), order.price, order.quantity,
      order.left_quantity, order.filled_quantity,
      PriceText(order.average_fill_price), order.fee, order.status,
      order.source_label, IdText(order.exchange_order_id),
      IdText(order.local_order_id), order.updated_time));
  if (selected) {
    return row | ftxui::inverted;
  }
  if (order.side_value >= 0) {
    return row | ftxui::color(ftxui::Color::Green);
  }
  return row | ftxui::color(ftxui::Color::Red);
}

inline ftxui::Element MarketDataRowElement(std::size_t visible_index,
                                           const MarketDataRow& row) {
  auto line = ftxui::text(fmt::format(
      "{:>3} {:<7} {:<11} {:<16} {:>10} {:>10} {:>10} {:>10} {:>10} {:>10} "
      "{:>12} {:<12}",
      visible_index, row.exchange, row.exchange_symbol, row.market_data_id,
      MarketNumberText(row, row.last_price, 2),
      MarketNumberText(row, row.bid_price, 2),
      MarketNumberText(row, row.bid_volume, 1),
      MarketNumberText(row, row.ask_price, 2),
      MarketNumberText(row, row.ask_volume, 1),
      MarketNumberText(row, row.volume, 1),
      MarketNumberText(row, row.turnover, 2), row.updated_time));
  if (!row.has_data) {
    return line | ftxui::color(ftxui::Color::GrayLight);
  }
  return line;
}

inline ftxui::Element SymbolPane(const AccountMonitorSnapshot& snapshot) {
  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({
      BoxTitle(fmt::format("SYMBOLS active {} / all {}",
                           snapshot.symbols.size(), snapshot.symbols.size())),
      ftxui::filler(),
      BoxTitle("risk sort"),
  }));
  rows.push_back(ftxui::separator());
  rows.push_back(ftxui::text("num symbol           pos open      pnl !") |
                 ftxui::color(ftxui::Color::GrayLight));
  for (std::size_t i = 0; i < snapshot.symbols.size(); ++i) {
    rows.push_back(SymbolRow(i + 1, snapshot.symbols[i]));
  }
  rows.push_back(ftxui::filler());
  rows.push_back(ftxui::separator());
  rows.push_back(ftxui::text("Legend: D drift, S stale, * selected") |
                 ftxui::color(ftxui::Color::GrayLight));
  rows.push_back(ftxui::text("Filter: active only | search: empty") |
                 ftxui::color(ftxui::Color::GrayLight));
  return ftxui::vbox(std::move(rows)) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 42);
}

inline ftxui::Element OrdersPane(const SymbolDetail& detail) {
  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({
      BoxTitle(fmt::format("ORDERS: {}", detail.symbol)),
      ftxui::filler(),
      BoxTitle(fmt::format("{} rows / source: all", detail.orders.size())),
  }));
  rows.push_back(ftxui::separator());
  rows.push_back(
      ftxui::text("num exch    symbol      side        px      qty     left"
                  "   filled      avg      fee status   source     exch_id"
                  "      local_id           updated") |
      ftxui::color(ftxui::Color::GrayLight));
  for (std::size_t i = 0; i < detail.orders.size(); ++i) {
    rows.push_back(OrderRow(i + 1, detail.orders[i], i == 1));
  }
  rows.push_back(ftxui::filler());
  return ftxui::vbox(std::move(rows)) | ftxui::flex;
}

inline ftxui::Element MarketDataPane(const SymbolDetail& detail) {
  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({
      BoxTitle(fmt::format("MARKET BY EXCHANGE: {}", detail.symbol)),
      ftxui::filler(),
      BoxTitle(
          fmt::format("{} rows / placeholder data", detail.market_data.size())),
  }));
  rows.push_back(ftxui::separator());
  rows.push_back(
      ftxui::text("num exch    symbol      id                 last        bid"
                  "    bid_vol        ask    ask_vol        vol     turnover"
                  " updated") |
      ftxui::color(ftxui::Color::GrayLight));
  for (std::size_t i = 0; i < detail.market_data.size(); ++i) {
    rows.push_back(MarketDataRowElement(i + 1, detail.market_data[i]));
  }
  return ftxui::vbox(std::move(rows));
}

inline ftxui::Element MiddlePane(const SymbolDetail& detail) {
  return ftxui::vbox({
             MarketDataPane(detail),
             ftxui::separator(),
             OrdersPane(detail) | ftxui::flex,
         }) |
         ftxui::flex;
}

inline ftxui::Element BalancePane(const AccountBalance& balance) {
  return ftxui::hbox({
             BoxTitle("BALANCE"),
             ftxui::text("  total "),
             ftxui::text(fmt::format("{:.2f} {}", balance.total_equity,
                                     balance.currency)) |
                 ftxui::bold,
             ftxui::text(" | available "),
             ftxui::text(fmt::format("{:.2f}", balance.available)) |
                 ftxui::color(ftxui::Color::Green),
             ftxui::text(" | used "),
             ftxui::text(fmt::format("{:.2f}", balance.used_margin)),
             ftxui::text(" | pnl "),
             ftxui::text(fmt::format("{:+.2f}", balance.total_pnl)) |
                 ftxui::bold | ftxui::color(PnlColor(balance.total_pnl)),
             ftxui::text(" | realized "),
             ftxui::text(fmt::format("{:+.2f}", balance.realized_pnl)) |
                 ftxui::color(PnlColor(balance.realized_pnl)),
             ftxui::text(" | unrealized "),
             ftxui::text(fmt::format("{:+.2f}", balance.unrealized_pnl)) |
                 ftxui::color(PnlColor(balance.unrealized_pnl)),
         }) |
         ftxui::border;
}

inline ftxui::Element PositionPane(const SymbolDetail& detail) {
  const PositionPnl& pnl = detail.position;
  const SourceMix& mix = detail.source_mix;
  ftxui::Elements rows;
  rows.push_back(BoxTitle(fmt::format("POSITION / PNL: {}", detail.symbol)));
  rows.push_back(
      ftxui::vbox({
          ftxui::text(fmt::format("net position  {:+.1f}", pnl.net_position)) |
              ftxui::bold | ftxui::color(PnlColor(pnl.net_position)),
          ftxui::text(fmt::format("open orders   {}", detail.orders.size())),
          ftxui::text(fmt::format("average entry {:.2f}", pnl.average_entry)),
          ftxui::text(fmt::format("mark price    {:.2f}", pnl.mark_price)),
          ftxui::text(fmt::format("notional      {:.2f}", pnl.notional)),
          ftxui::text(fmt::format("exposure      {}", pnl.exposure)),
      }) |
      ftxui::border);
  rows.push_back(
      ftxui::vbox({
          ftxui::text(fmt::format("realized pnl  {:+.2f}", pnl.realized_pnl)) |
              ftxui::color(PnlColor(pnl.realized_pnl)),
          ftxui::text(
              fmt::format("unrealized pnl {:+.2f}", pnl.unrealized_pnl)) |
              ftxui::color(PnlColor(pnl.unrealized_pnl)),
          ftxui::text(fmt::format("fees          {:+.2f}", pnl.fees)) |
              ftxui::color(PnlColor(pnl.fees)),
          ftxui::text(fmt::format("total pnl     {:+.2f}", pnl.total_pnl)) |
              ftxui::bold | ftxui::color(PnlColor(pnl.total_pnl)),
      }) |
      ftxui::border);
  rows.push_back(BoxTitle("SOURCE MIX / HEALTH"));
  rows.push_back(
      ftxui::vbox({
          ftxui::text(fmt::format("Aquila orders {}", mix.aquila)),
          ftxui::text(fmt::format("Manual orders {}", mix.manual)),
          ftxui::text(fmt::format("External orders {}", mix.external)),
          ftxui::text(fmt::format("Unknown orders {}", mix.unknown)),
          ftxui::separator(),
          ftxui::text(
              fmt::format("ws updates    {}s ago", pnl.ws_update_age_seconds)),
          ftxui::text(fmt::format("rest snapshot {}s ago",
                                  pnl.rest_snapshot_age_seconds)),
          ftxui::text(fmt::format("ledger        {}", pnl.ledger_state)),
          ftxui::text(fmt::format("drift         {}", pnl.drift_state)),
      }) |
      ftxui::border);
  rows.push_back(BoxTitle("SELECTED ORDER"));
  if (detail.orders.size() > 1) {
    const MonitorOrder& order = detail.orders[1];
    rows.push_back(
        ftxui::vbox({
            ftxui::text(fmt::format("exch          {}", order.exchange)),
            ftxui::text(fmt::format("symbol        {}", order.exchange_symbol)),
            ftxui::text(fmt::format("exch_id       {}",
                                    IdText(order.exchange_order_id))),
            ftxui::text(
                fmt::format("local_id      {}", IdText(order.local_order_id))),
            ftxui::text(fmt::format("side          {}",
                                    SideValueText(order.side_value))),
            ftxui::text(fmt::format("quantity/left {:.1f}/{:.1f}",
                                    order.quantity, order.left_quantity)),
            ftxui::text(fmt::format("price         {:.2f}", order.price)),
            ftxui::text(fmt::format("updated       {}", order.updated_time)),
            ftxui::text(fmt::format("source        {}", order.source_label)),
            ftxui::text(fmt::format("status        {}", order.status)),
        }) |
        ftxui::border);
  }
  return ftxui::vbox(std::move(rows)) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 52);
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
                 ftxui::text(
                     "  q/esc quit | / search | r refresh | f active/all") |
                     ftxui::color(ftxui::Color::GrayLight),
             }),
             symbol_workbench_view_detail::BalancePane(snapshot.balance),
             ftxui::separator(),
             ftxui::hbox({
                 symbol_workbench_view_detail::SymbolPane(snapshot),
                 ftxui::separator(),
                 symbol_workbench_view_detail::MiddlePane(*detail),
                 ftxui::separator(),
                 symbol_workbench_view_detail::PositionPane(*detail),
             }) | ftxui::flex,
             symbol_workbench_view_detail::EventsPane(snapshot),
         }) |
         ftxui::border;
}

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_TUI_SYMBOL_WORKBENCH_VIEW_H_
