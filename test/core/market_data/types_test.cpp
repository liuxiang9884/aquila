#include "core/common/constants.h"
#include "core/market_data/types.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

TEST(CoreMarketDataTypesTest, BookTickerCarriesExchangeAndTopOfBook) {
  static_assert(std::is_standard_layout_v<aquila::BookTicker>);
  static_assert(std::is_trivially_copyable_v<aquila::BookTicker>);
  static_assert(alignof(aquila::BookTicker) == alignof(double));
  static_assert(sizeof(aquila::BookTicker) == aquila::kCacheLineBytes);
  static_assert(offsetof(aquila::BookTicker, id) == 0);
  static_assert(offsetof(aquila::BookTicker, symbol_id) == 8);
  static_assert(offsetof(aquila::BookTicker, exchange) == 12);
  static_assert(offsetof(aquila::BookTicker, exchange_ns) == 16);
  static_assert(offsetof(aquila::BookTicker, local_ns) == 24);
  static_assert(offsetof(aquila::BookTicker, bid_price) == 32);
  static_assert(offsetof(aquila::BookTicker, bid_volume) == 40);
  static_assert(offsetof(aquila::BookTicker, ask_price) == 48);
  static_assert(offsetof(aquila::BookTicker, ask_volume) == 56);

  const aquila::BookTicker book_ticker{
      .id = 9'876'543,
      .symbol_id = 42,
      .exchange = aquila::Exchange::kGate,
      .exchange_ns = 1'770'000'000'000'900'000,
      .local_ns = 1'770'000'000'001'200'000,
      .bid_price = 65'012.0,
      .bid_volume = 21.5,
      .ask_price = 65'012.5,
      .ask_volume = 17.25,
  };

  EXPECT_EQ(book_ticker.id, 9'876'543);
  EXPECT_EQ(book_ticker.symbol_id, 42);
  EXPECT_EQ(book_ticker.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(book_ticker.exchange_ns, 1'770'000'000'000'900'000);
  EXPECT_EQ(book_ticker.local_ns, 1'770'000'000'001'200'000);
  EXPECT_DOUBLE_EQ(book_ticker.bid_price, 65'012.0);
  EXPECT_DOUBLE_EQ(book_ticker.bid_volume, 21.5);
  EXPECT_DOUBLE_EQ(book_ticker.ask_price, 65'012.5);
  EXPECT_DOUBLE_EQ(book_ticker.ask_volume, 17.25);
}

}  // namespace
