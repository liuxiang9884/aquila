#include "core/market_data/types.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

namespace {

TEST(CoreMarketDataTypesTest, BookTickerCarriesExchangeAndTopOfBook) {
  static_assert(std::is_standard_layout_v<aquila::BookTicker>);
  static_assert(std::is_trivially_copyable_v<aquila::BookTicker>);

  const aquila::BookTicker book_ticker{
      .exchange = aquila::Exchange::kGate,
      .symbol_id = 42,
      .exchange_time_ns = 1'770'000'000'000'900'000,
      .local_time_ns = 1'770'000'000'001'200'000,
      .elapsed_ns = 12'000,
      .sequence = 9'876'543,
      .bid_price = 65'012.0,
      .bid_volume = 21.5,
      .ask_price = 65'012.5,
      .ask_volume = 17.25,
  };

  EXPECT_EQ(book_ticker.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(book_ticker.symbol_id, 42);
  EXPECT_EQ(book_ticker.exchange_time_ns, 1'770'000'000'000'900'000);
  EXPECT_EQ(book_ticker.local_time_ns, 1'770'000'000'001'200'000);
  EXPECT_EQ(book_ticker.elapsed_ns, 12'000);
  EXPECT_EQ(book_ticker.sequence, 9'876'543);
  EXPECT_DOUBLE_EQ(book_ticker.bid_price, 65'012.0);
  EXPECT_DOUBLE_EQ(book_ticker.bid_volume, 21.5);
  EXPECT_DOUBLE_EQ(book_ticker.ask_price, 65'012.5);
  EXPECT_DOUBLE_EQ(book_ticker.ask_volume, 17.25);
}

}  // namespace
