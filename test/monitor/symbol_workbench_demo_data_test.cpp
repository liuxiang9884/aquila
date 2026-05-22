#include "monitor/demo/symbol_workbench_demo_data.h"

#include <string_view>

#include <gtest/gtest.h>

namespace aquila::monitor {
namespace {

TEST(SymbolWorkbenchDemoDataTest, ContainsRequestedSymbolsInOrder) {
  const auto symbols = DemoSymbolSummaries();
  ASSERT_EQ(symbols.size(), 11U);

  constexpr std::string_view kExpected[] = {
      "ROVE_USDT",  "RAVE_USDT",  "ZEC_USDT",   "SIREN_USDT",
      "ETC_USDT",   "DASH_USDT",  "RIVER_USDT", "SUI_USDT",
      "INJ_USDT",   "ENA_USDT",   "BRETT_USDT",
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

TEST(SymbolWorkbenchDemoDataTest, ZecTotalPnlIncludesFees) {
  const SymbolDetail* detail = DemoSelectedSymbolDetail();
  ASSERT_NE(detail, nullptr);

  const PositionPnl& pnl = detail->position;
  EXPECT_DOUBLE_EQ(pnl.total_pnl, pnl.realized_pnl + pnl.unrealized_pnl +
                                      pnl.fees);
}

}  // namespace
}  // namespace aquila::monitor
