#include "core/market_data/book_ticker_fusion.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace {

aquila::BookTicker MakeTicker(std::int32_t symbol_id, std::int64_t id,
                              std::int64_t source_local_ns) {
  return aquila::BookTicker{
      .id = id,
      .symbol_id = symbol_id,
      .exchange = aquila::Exchange::kGate,
      .exchange_ns = 1'780'000'000'000'000'000 + id,
      .local_ns = source_local_ns,
      .bid_price = 10.0 + static_cast<double>(id),
      .bid_volume = 1.0 + static_cast<double>(symbol_id),
      .ask_price = 11.0 + static_cast<double>(id),
      .ask_volume = 2.0 + static_cast<double>(symbol_id),
  };
}

void ExpectPublishedTicker(const aquila::market_data::BookTickerFusionDecision& decision,
                           const aquila::BookTicker& source,
                           std::int64_t fusion_publish_ns,
                           std::int32_t source_id) {
  ASSERT_TRUE(decision.publish);
  EXPECT_EQ(decision.source_id, source_id);
  EXPECT_EQ(decision.symbol_id, source.symbol_id);
  EXPECT_EQ(decision.book_ticker_id, source.id);
  EXPECT_EQ(decision.source_local_ns, source.local_ns);
  EXPECT_EQ(decision.fusion_publish_ns, fusion_publish_ns);
  EXPECT_EQ(decision.ticker.id, source.id);
  EXPECT_EQ(decision.ticker.symbol_id, source.symbol_id);
  EXPECT_EQ(decision.ticker.exchange, source.exchange);
  EXPECT_EQ(decision.ticker.exchange_ns, source.exchange_ns);
  EXPECT_EQ(decision.ticker.local_ns, fusion_publish_ns);
  EXPECT_DOUBLE_EQ(decision.ticker.bid_price, source.bid_price);
  EXPECT_DOUBLE_EQ(decision.ticker.bid_volume, source.bid_volume);
  EXPECT_DOUBLE_EQ(decision.ticker.ask_price, source.ask_price);
  EXPECT_DOUBLE_EQ(decision.ticker.ask_volume, source.ask_volume);
}

TEST(BookTickerFusionCoreTest, PublishesOnlyIncreasingIdsPerSymbol) {
  aquila::market_data::BookTickerFusionCore fusion(/*max_symbol_id=*/128);

  const aquila::BookTicker first = MakeTicker(42, 100, 1'000);
  ExpectPublishedTicker(
      fusion.OnBookTicker(/*source_id=*/0, first, /*fusion_publish_ns=*/2'000),
      first, 2'000, 0);

  const aquila::BookTicker duplicate = MakeTicker(42, 100, 1'100);
  EXPECT_FALSE(
      fusion.OnBookTicker(/*source_id=*/1, duplicate,
                          /*fusion_publish_ns=*/2'100)
          .publish);

  const aquila::BookTicker older = MakeTicker(42, 99, 1'200);
  EXPECT_FALSE(
      fusion.OnBookTicker(/*source_id=*/2, older, /*fusion_publish_ns=*/2'200)
          .publish);

  const aquila::BookTicker next = MakeTicker(42, 101, 1'300);
  ExpectPublishedTicker(
      fusion.OnBookTicker(/*source_id=*/3, next, /*fusion_publish_ns=*/2'300),
      next, 2'300, 3);
}

TEST(BookTickerFusionCoreTest, MaintainsIndependentStatePerSymbol) {
  aquila::market_data::BookTickerFusionCore fusion(/*max_symbol_id=*/128);

  const aquila::BookTicker symbol_one = MakeTicker(1, 10, 3'000);
  ExpectPublishedTicker(
      fusion.OnBookTicker(/*source_id=*/0, symbol_one,
                          /*fusion_publish_ns=*/4'000),
      symbol_one, 4'000, 0);

  const aquila::BookTicker symbol_two = MakeTicker(2, 5, 3'100);
  ExpectPublishedTicker(
      fusion.OnBookTicker(/*source_id=*/1, symbol_two,
                          /*fusion_publish_ns=*/4'100),
      symbol_two, 4'100, 1);

  const aquila::BookTicker stale_symbol_one = MakeTicker(1, 9, 3'200);
  EXPECT_FALSE(fusion
                   .OnBookTicker(/*source_id=*/2, stale_symbol_one,
                                 /*fusion_publish_ns=*/4'200)
                   .publish);
}

TEST(BookTickerFusionCoreTest, DropsOutOfRangeSymbolId) {
  aquila::market_data::BookTickerFusionCore fusion(/*max_symbol_id=*/8);

  EXPECT_FALSE(fusion
                   .OnBookTicker(/*source_id=*/0, MakeTicker(9, 1, 5'000),
                                 /*fusion_publish_ns=*/6'000)
                   .publish);
  EXPECT_FALSE(fusion
                   .OnBookTicker(/*source_id=*/0, MakeTicker(-1, 1, 5'100),
                                 /*fusion_publish_ns=*/6'100)
                   .publish);
}

}  // namespace
