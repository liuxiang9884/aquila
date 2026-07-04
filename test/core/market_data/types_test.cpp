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

TEST(CoreMarketDataTypesTest, TradeCarriesGatePublicTradeEvent) {
  static_assert(std::is_standard_layout_v<aquila::Trade>);
  static_assert(std::is_trivially_copyable_v<aquila::Trade>);
  static_assert(alignof(aquila::Trade) == alignof(double));
  static_assert(sizeof(aquila::Trade) == aquila::kCacheLineBytes);
  static_assert(offsetof(aquila::Trade, id) == 0);
  static_assert(offsetof(aquila::Trade, symbol_id) == 8);
  static_assert(offsetof(aquila::Trade, exchange) == 12);
  static_assert(offsetof(aquila::Trade, side) == 13);
  static_assert(offsetof(aquila::Trade, reserved) == 14);
  static_assert(offsetof(aquila::Trade, exchange_ns) == 16);
  static_assert(offsetof(aquila::Trade, trade_ns) == 24);
  static_assert(offsetof(aquila::Trade, local_ns) == 32);
  static_assert(offsetof(aquila::Trade, price) == 40);
  static_assert(offsetof(aquila::Trade, volume) == 48);
  static_assert(offsetof(aquila::Trade, batch_index) == 56);
  static_assert(offsetof(aquila::Trade, batch_count) == 60);

  const aquila::Trade trade{
      .id = 123456789,
      .symbol_id = 42,
      .exchange = aquila::Exchange::kGate,
      .side = aquila::OrderSide::kBuy,
      .reserved = 0,
      .exchange_ns = 1'770'000'000'001'000'000,
      .trade_ns = 1'770'000'000'000'990'000,
      .local_ns = 1'770'000'000'001'200'000,
      .price = 65'012.5,
      .volume = 17.5,
      .batch_index = 1,
      .batch_count = 3,
  };

  EXPECT_EQ(trade.id, 123456789);
  EXPECT_EQ(trade.symbol_id, 42);
  EXPECT_EQ(trade.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(trade.side, aquila::OrderSide::kBuy);
  EXPECT_EQ(trade.reserved, 0);
  EXPECT_EQ(trade.exchange_ns, 1'770'000'000'001'000'000);
  EXPECT_EQ(trade.trade_ns, 1'770'000'000'000'990'000);
  EXPECT_EQ(trade.local_ns, 1'770'000'000'001'200'000);
  EXPECT_DOUBLE_EQ(trade.price, 65'012.5);
  EXPECT_DOUBLE_EQ(trade.volume, 17.5);
  EXPECT_EQ(trade.batch_index, 1U);
  EXPECT_EQ(trade.batch_count, 3U);
}

}  // namespace
