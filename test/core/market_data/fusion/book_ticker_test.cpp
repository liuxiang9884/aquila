#include "core/market_data/fusion/book_ticker.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace {

template <typename T>
concept HasDecisionTicker = requires(const T& decision) { decision.ticker; };

static_assert(
    !HasDecisionTicker<aquila::market_data::BookTickerFusionDecision>);

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

void ExpectPublishedDecision(
    const aquila::market_data::BookTickerFusionDecision& decision,
    const aquila::BookTicker& source, std::int64_t fusion_publish_ns,
    std::int32_t source_id) {
  ASSERT_TRUE(decision.publish);
  EXPECT_EQ(decision.source_id, source_id);
  EXPECT_EQ(decision.symbol_id, source.symbol_id);
  EXPECT_EQ(decision.record_id, source.id);
  EXPECT_EQ(decision.source_local_ns, source.local_ns);
  EXPECT_EQ(decision.fusion_publish_ns, fusion_publish_ns);
}

TEST(BookTickerFusionCoreTest, PublishesOnlyIncreasingIdsPerSymbol) {
  aquila::market_data::BookTickerFusionCore fusion(/*max_symbol_id=*/128);

  const aquila::BookTicker first = MakeTicker(42, 100, 1'000);
  ExpectPublishedDecision(
      fusion.OnBookTicker(/*source_id=*/0, first, /*fusion_publish_ns=*/2'000),
      first, 2'000, 0);

  const aquila::BookTicker duplicate = MakeTicker(42, 100, 1'100);
  EXPECT_FALSE(fusion
                   .OnBookTicker(/*source_id=*/1, duplicate,
                                 /*fusion_publish_ns=*/2'100)
                   .publish);

  const aquila::BookTicker older = MakeTicker(42, 99, 1'200);
  EXPECT_FALSE(
      fusion.OnBookTicker(/*source_id=*/2, older, /*fusion_publish_ns=*/2'200)
          .publish);

  const aquila::BookTicker next = MakeTicker(42, 101, 1'300);
  ExpectPublishedDecision(
      fusion.OnBookTicker(/*source_id=*/3, next, /*fusion_publish_ns=*/2'300),
      next, 2'300, 3);
}

TEST(BookTickerFusionCoreTest, MaintainsIndependentStatePerSymbol) {
  aquila::market_data::BookTickerFusionCore fusion(/*max_symbol_id=*/128);

  const aquila::BookTicker symbol_one = MakeTicker(1, 10, 3'000);
  ExpectPublishedDecision(fusion.OnBookTicker(/*source_id=*/0, symbol_one,
                                              /*fusion_publish_ns=*/4'000),
                          symbol_one, 4'000, 0);

  const aquila::BookTicker symbol_two = MakeTicker(2, 5, 3'100);
  ExpectPublishedDecision(fusion.OnBookTicker(/*source_id=*/1, symbol_two,
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

TEST(BookTickerFusionCoreTest, DropDecisionsKeepMetadataUnset) {
  aquila::market_data::BookTickerFusionCore fusion(/*max_symbol_id=*/8);

  const aquila::BookTicker first = MakeTicker(2, 10, 7'000);
  ExpectPublishedDecision(
      fusion.OnBookTicker(/*source_id=*/0, first, /*fusion_publish_ns=*/8'000),
      first, 8'000, 0);

  const aquila::market_data::BookTickerFusionDecision stale_decision =
      fusion.OnBookTicker(/*source_id=*/1, MakeTicker(2, 10, 7'100),
                          /*fusion_publish_ns=*/8'100);
  EXPECT_FALSE(stale_decision.publish);
  EXPECT_EQ(stale_decision.source_id, -1);
  EXPECT_EQ(stale_decision.symbol_id, -1);
  EXPECT_EQ(stale_decision.record_id, 0);
  EXPECT_EQ(stale_decision.source_local_ns, 0);
  EXPECT_EQ(stale_decision.fusion_publish_ns, 0);

  const aquila::market_data::BookTickerFusionDecision invalid_decision =
      fusion.OnBookTicker(/*source_id=*/2, MakeTicker(9, 11, 7'200),
                          /*fusion_publish_ns=*/8'200);
  EXPECT_FALSE(invalid_decision.publish);
  EXPECT_EQ(invalid_decision.source_id, -1);
  EXPECT_EQ(invalid_decision.symbol_id, -1);
  EXPECT_EQ(invalid_decision.record_id, 0);
  EXPECT_EQ(invalid_decision.source_local_ns, 0);
  EXPECT_EQ(invalid_decision.fusion_publish_ns, 0);
}

}  // namespace
