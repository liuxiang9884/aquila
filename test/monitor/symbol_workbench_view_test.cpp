#include "monitor/tui/symbol_workbench_view.h"

#include <initializer_list>
#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

#include "monitor/demo/symbol_workbench_demo_data.h"
#include "monitor/tui/runtime_health_view.h"

namespace aquila::monitor {
namespace {

std::string RenderToString(ftxui::Element element, int width, int height) {
  ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                               ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, element);
  return screen.ToString();
}

void ExpectTokensInOrder(std::string_view text,
                         std::initializer_list<std::string_view> tokens) {
  std::size_t position = 0;
  for (std::string_view token : tokens) {
    const std::size_t found = text.find(token, position);
    EXPECT_NE(found, std::string_view::npos) << "missing token: " << token;
    if (found == std::string_view::npos) {
      return;
    }
    position = found + token.size();
  }
}

TEST(SymbolWorkbenchViewTest, MarketPaneUsesCompactHeadersAndNaPlaceholder) {
  const SymbolDetail* detail = DemoSelectedSymbolDetail();
  ASSERT_NE(detail, nullptr);

  const std::string rendered = RenderToString(
      symbol_workbench_view_detail::MarketDataPane(*detail), 220, 8);

  EXPECT_EQ(rendered.find("market_data_id"), std::string::npos);
  EXPECT_EQ(rendered.find("not available"), std::string::npos);
  EXPECT_NE(rendered.find("NA"), std::string::npos);
  ExpectTokensInOrder(
      rendered, {"num", "exch", "symbol", "id", "last", "bid", "bid_vol", "ask",
                 "ask_vol", "vol", "turnover", "updated"});
  ExpectTokensInOrder(rendered, {"1", "Gate", "2", "Binance", "3", "OKX"});
}

TEST(SymbolWorkbenchViewTest, OrdersPaneMovesIdsBeforeUpdated) {
  const SymbolDetail* detail = DemoSelectedSymbolDetail();
  ASSERT_NE(detail, nullptr);

  const std::string rendered = RenderToString(
      symbol_workbench_view_detail::OrdersPane(*detail), 260, 12);

  EXPECT_EQ(rendered.find("not available"), std::string::npos);
  ExpectTokensInOrder(rendered, {"num", "exch", "symbol", "side", "px", "qty",
                                 "left", "filled", "avg", "fee", "status",
                                 "source", "exch_id", "local_id", "updated"});
  ExpectTokensInOrder(rendered, {"2", "Gate", "ZEC_USDT", "-1", "63.40", "2.0",
                                 "2.0", "0.0", "NA", "0.00", "open", "Manual",
                                 "9138472901", "-", "11:40:05.442"});
}

TEST(SymbolWorkbenchViewTest, SymbolPaneUsesVisibleRowNumbers) {
  const AccountMonitorSnapshot snapshot = DemoAccountMonitorSnapshot();

  const std::string rendered = RenderToString(
      symbol_workbench_view_detail::SymbolPane(snapshot), 60, 18);

  ExpectTokensInOrder(rendered, {"num", "symbol", "pos", "open", "pnl"});
  ExpectTokensInOrder(rendered,
                      {"1", "PROVE_USDT", "2", "RAVE_USDT", "3", "ZEC_USDT"});
}

TEST(SymbolWorkbenchViewTest, FullWorkbenchRendersBalanceStripUnderTopBar) {
  const AccountMonitorSnapshot snapshot = DemoAccountMonitorSnapshot();

  const std::string rendered =
      RenderToString(RenderSymbolWorkbench(snapshot), 260, 40);

  ExpectTokensInOrder(rendered, {"AQUILA GATE USDT ACCOUNT", "BALANCE", "total",
                                 "12548.72 USDT", "available", "9876.54", "pnl",
                                 "+42.68", "SYMBOLS"});
  const std::size_t symbols_position = rendered.find("SYMBOLS");
  const std::size_t health_position = rendered.find("server ok");
  ASSERT_NE(symbols_position, std::string::npos);
  ASSERT_NE(health_position, std::string::npos);
  EXPECT_GT(health_position, symbols_position);
}

TEST(SymbolWorkbenchViewTest, EventsPaneShowsOnlyFiveRows) {
  const AccountMonitorSnapshot snapshot = DemoAccountMonitorSnapshot();

  const std::string rendered = RenderToString(
      symbol_workbench_view_detail::EventsPane(snapshot), 180, 9);

  ExpectTokensInOrder(rendered, {"EVENTS", "11:40:02", "11:40:05", "11:40:09",
                                 "11:40:12", "11:40:14"});
  EXPECT_EQ(rendered.find("11:40:16"), std::string::npos);
}

TEST(SymbolWorkbenchViewTest, BottomPaneSplitsEventsAndHealthAlerts) {
  const AccountMonitorSnapshot snapshot = DemoAccountMonitorSnapshot();

  const std::string rendered = RenderToString(
      symbol_workbench_view_detail::BottomPane(snapshot), 260, 12);

  ExpectTokensInOrder(rendered,
                      {"EVENTS", "HEALTH", "server ok", "ALERTS", "warning",
                       "disk_root", "error", "gate_order_feedback_session"});
}

TEST(SymbolWorkbenchViewTest, HealthSummaryStripRendersProcessCounts) {
  const AccountMonitorSnapshot snapshot = DemoAccountMonitorSnapshot();

  const std::string rendered =
      RenderToString(RenderHealthSummaryStrip(snapshot.runtime_health), 180, 3);

  ExpectTokensInOrder(rendered,
                      {"HEALTH", "server ok", "cpu 18%", "mem 42%", "disk 71%",
                       "md 2/2 up", "trading 3/3 up", "stale 1", "restart 0"});
}

TEST(SymbolWorkbenchViewTest, HealthPageRendersServerProcessesAndConnections) {
  const AccountMonitorSnapshot snapshot = DemoAccountMonitorSnapshot();

  const std::string rendered =
      RenderToString(RenderRuntimeHealthPage(snapshot), 260, 42);

  ExpectTokensInOrder(
      rendered, {"AQUILA RUNTIME HEALTH", "SERVER", "metric", "value", "state",
                 "updated", "disk_root", "warn"});
  ExpectTokensInOrder(rendered, {"PROCESSES",
                                 "num",
                                 "name",
                                 "role",
                                 "pid",
                                 "uptime",
                                 "cpu",
                                 "mem",
                                 "status",
                                 "heartbeat",
                                 "updated",
                                 "1",
                                 "gate_data_session",
                                 "market",
                                 "2",
                                 "binance_data_session",
                                 "market",
                                 "3",
                                 "lead_lag_strategy",
                                 "trading",
                                 "4",
                                 "gate_order_feedback_session",
                                 "feedback",
                                 "stale"});
  ExpectTokensInOrder(
      rendered,
      {"CONNECTIONS", "num", "venue", "channel", "state", "latency", "last_msg",
       "reconnects", "updated", "Gate", "futures.orders", "connected"});
}

}  // namespace
}  // namespace aquila::monitor
