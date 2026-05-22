#include "monitor/tui/symbol_workbench_view.h"

#include <initializer_list>
#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

#include "monitor/demo/symbol_workbench_demo_data.h"

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
  ExpectTokensInOrder(rendered,
                      {"exch", "symbol", "id", "last", "bid", "bid_vol", "ask",
                       "ask_vol", "vol", "turnover", "updated"});
}

TEST(SymbolWorkbenchViewTest, OrdersPaneMovesIdsBeforeUpdated) {
  const SymbolDetail* detail = DemoSelectedSymbolDetail();
  ASSERT_NE(detail, nullptr);

  const std::string rendered = RenderToString(
      symbol_workbench_view_detail::OrdersPane(*detail), 260, 12);

  EXPECT_EQ(rendered.find("not available"), std::string::npos);
  ExpectTokensInOrder(
      rendered, {"exch", "symbol", "side", "px", "qty", "left", "filled", "avg",
                 "fee", "status", "source", "exch_id", "local_id", "updated"});
  ExpectTokensInOrder(
      rendered, {"Gate", "ZEC_USDT", "-1", "63.40", "2.0", "2.0", "0.0", "NA",
                 "0.00", "open", "Manual", "9138472901", "-", "11:40:05.442"});
}

}  // namespace
}  // namespace aquila::monitor
