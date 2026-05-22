#include "monitor/demo/symbol_workbench_demo_data.h"

#include <string_view>

#include <gtest/gtest.h>

namespace aquila::monitor {
namespace {

TEST(SymbolWorkbenchDemoDataTest, ContainsRequestedSymbolsInOrder) {
  const auto symbols = DemoSymbolSummaries();
  ASSERT_EQ(symbols.size(), 11U);

  constexpr std::string_view kExpected[] = {
      "ROVE_USDT", "RAVE_USDT", "ZEC_USDT",   "SIREN_USDT",
      "ETC_USDT",  "DASH_USDT", "RIVER_USDT", "SUI_USDT",
      "INJ_USDT",  "ENA_USDT",  "BRETT_USDT",
  };
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    EXPECT_EQ(symbols[i].symbol, kExpected[i]);
  }
}

TEST(SymbolWorkbenchDemoDataTest, SelectsZecByDefault) {
  const SymbolDetail* detail = DemoSelectedSymbolDetail();

  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(DemoSelectedSymbol(), "ZEC_USDT");
  EXPECT_EQ(detail->symbol, "ZEC_USDT");
  EXPECT_EQ(detail->position.net_position, 3.0);
  EXPECT_EQ(detail->orders.size(), 6U);
  EXPECT_EQ(detail->market_data.size(), 3U);
}

TEST(SymbolWorkbenchDemoDataTest, ExposesAccountBalanceSummary) {
  const AccountMonitorSnapshot snapshot = DemoAccountMonitorSnapshot();

  EXPECT_EQ(snapshot.balance.currency, "USDT");
  EXPECT_DOUBLE_EQ(snapshot.balance.total_equity, 12548.72);
  EXPECT_DOUBLE_EQ(snapshot.balance.available, 9876.54);
  EXPECT_DOUBLE_EQ(snapshot.balance.total_pnl, 42.68);
}

TEST(SymbolWorkbenchDemoDataTest, ExposesRuntimeHealthSummary) {
  const AccountMonitorSnapshot snapshot = DemoAccountMonitorSnapshot();

  EXPECT_EQ(snapshot.runtime_health.server_state, "ok");
  EXPECT_DOUBLE_EQ(snapshot.runtime_health.cpu_percent, 18.0);
  EXPECT_EQ(snapshot.runtime_health.market_processes_up, 2);
  EXPECT_EQ(snapshot.runtime_health.market_processes_total, 2);
  EXPECT_EQ(snapshot.runtime_health.trading_processes_up, 3);
  EXPECT_EQ(snapshot.runtime_health.trading_processes_total, 3);
  EXPECT_EQ(snapshot.runtime_health.stale_count, 1);
  EXPECT_EQ(snapshot.runtime_health.server_metrics.size(), 5U);
  EXPECT_EQ(snapshot.runtime_health.processes.size(), 4U);
  EXPECT_EQ(snapshot.runtime_health.connections.size(), 3U);
}

TEST(SymbolWorkbenchDemoDataTest, ZecOrdersExposeAllSourceClasses) {
  const SymbolDetail* detail = DemoSelectedSymbolDetail();
  ASSERT_NE(detail, nullptr);

  int aquila_count = 0;
  int manual_count = 0;
  int external_count = 0;
  for (const MonitorOrder& order : detail->orders) {
    if (order.source == OrderSource::kAquila) {
      ++aquila_count;
    } else if (order.source == OrderSource::kManual) {
      ++manual_count;
    } else if (order.source == OrderSource::kExternal) {
      ++external_count;
    }
  }

  EXPECT_EQ(aquila_count, 2);
  EXPECT_EQ(manual_count, 2);
  EXPECT_EQ(external_count, 2);
}

TEST(SymbolWorkbenchDemoDataTest, ZecOrdersExposeExchangeIdsAndUpdateTimes) {
  const SymbolDetail* detail = DemoSelectedSymbolDetail();
  ASSERT_NE(detail, nullptr);
  ASSERT_GE(detail->orders.size(), 2U);

  const MonitorOrder& aquila_order = detail->orders[0];
  EXPECT_EQ(aquila_order.exchange, "Gate");
  EXPECT_EQ(aquila_order.exchange_symbol, "ZEC_USDT");
  EXPECT_EQ(aquila_order.exchange_order_id, 9138472801U);
  EXPECT_EQ(aquila_order.local_order_id, 144115188075855874U);
  EXPECT_EQ(aquila_order.side_value, 1);
  EXPECT_EQ(aquila_order.updated_time, "11:40:02.118");

  const MonitorOrder& manual_order = detail->orders[1];
  EXPECT_EQ(manual_order.exchange, "Gate");
  EXPECT_EQ(manual_order.exchange_symbol, "ZEC_USDT");
  EXPECT_EQ(manual_order.local_order_id, 0U);
  EXPECT_EQ(manual_order.side_value, -1);
  EXPECT_EQ(manual_order.updated_time, "11:40:05.442");
}

TEST(SymbolWorkbenchDemoDataTest, ZecMarketDataUsesExchangeRows) {
  const SymbolDetail* detail = DemoSelectedSymbolDetail();
  ASSERT_NE(detail, nullptr);
  ASSERT_EQ(detail->market_data.size(), 3U);

  const MarketDataRow& gate = detail->market_data[0];
  EXPECT_EQ(gate.exchange, "Gate");
  EXPECT_EQ(gate.exchange_symbol, "ZEC_USDT");
  EXPECT_EQ(gate.market_data_id, "gate-zec-9841");
  EXPECT_DOUBLE_EQ(gate.last_price, 62.86);
  EXPECT_DOUBLE_EQ(gate.bid_price, 62.85);
  EXPECT_DOUBLE_EQ(gate.bid_volume, 18.4);
  EXPECT_DOUBLE_EQ(gate.ask_price, 62.86);
  EXPECT_DOUBLE_EQ(gate.ask_volume, 9.7);
  EXPECT_DOUBLE_EQ(gate.volume, 1240.6);
  EXPECT_DOUBLE_EQ(gate.turnover, 77982.12);
  EXPECT_EQ(gate.updated_time, "11:40:12.384");

  const MarketDataRow& okx = detail->market_data[2];
  EXPECT_EQ(okx.exchange, "OKX");
  EXPECT_EQ(okx.updated_time, "-");
}

TEST(SymbolWorkbenchDemoDataTest, ZecTotalPnlIncludesFees) {
  const SymbolDetail* detail = DemoSelectedSymbolDetail();
  ASSERT_NE(detail, nullptr);

  const PositionPnl& pnl = detail->position;
  EXPECT_DOUBLE_EQ(pnl.total_pnl,
                   pnl.realized_pnl + pnl.unrealized_pnl + pnl.fees);
}

}  // namespace
}  // namespace aquila::monitor
