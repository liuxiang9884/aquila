#include <type_traits>

#include <gtest/gtest.h>

#include "core/market_data/fusion/book_ticker.h"
#include "core/market_data/fusion/config.h"
#include "core/market_data/fusion/trade.h"
#include "tools/market_data/fusion_cli.h"

namespace {

namespace md = aquila::market_data;
namespace support = aquila::tools::market_data;

TEST(FusionCliTraitsTest, BindsBookTickerTypes) {
  EXPECT_EQ(support::BookTickerFusionCliTraits::kFeed,
            support::DataFusionFeed::kBookTicker);
  EXPECT_TRUE(
      (std::is_same_v<typename support::BookTickerFusionCliTraits::Config,
                      md::BookTickerFusionConfig>));
  EXPECT_TRUE(
      (std::is_same_v<typename support::BookTickerFusionCliTraits::Runner,
                      md::BookTickerFusionRunner>));
}

TEST(FusionCliTraitsTest, BindsTradeTypes) {
  EXPECT_EQ(support::TradeFusionCliTraits::kFeed,
            support::DataFusionFeed::kTrade);
  EXPECT_TRUE((std::is_same_v<typename support::TradeFusionCliTraits::Config,
                              md::TradeFusionConfig>));
  EXPECT_TRUE((std::is_same_v<typename support::TradeFusionCliTraits::Runner,
                              md::TradeFusionRunner>));
}

}  // namespace
