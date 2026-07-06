#include "core/market_data/trade_fusion.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace {

aquila::Trade MakeTrade(std::int32_t symbol_id, std::int64_t id,
                        std::int64_t source_local_ns) {
  return aquila::Trade{
      .id = id,
      .symbol_id = symbol_id,
      .exchange = aquila::Exchange::kGate,
      .side = aquila::OrderSide::kBuy,
      .reserved = 0,
      .exchange_ns = 1'780'000'000'000'000'000 + id,
      .trade_ns = 1'780'000'000'000'100'000 + id,
      .local_ns = source_local_ns,
      .price = 100.0 + static_cast<double>(id),
      .volume = 0.01 * static_cast<double>(symbol_id + 1),
      .batch_index = 1,
      .batch_count = 3,
  };
}

void ExpectPublishedDecision(const aquila::market_data::TradeFusionDecision& d,
                             const aquila::Trade& source,
                             std::int64_t fusion_publish_ns,
                             std::int32_t source_id) {
  ASSERT_TRUE(d.publish);
  EXPECT_EQ(d.source_id, source_id);
  EXPECT_EQ(d.symbol_id, source.symbol_id);
  EXPECT_EQ(d.trade_id, source.id);
  EXPECT_EQ(d.source_local_ns, source.local_ns);
  EXPECT_EQ(d.fusion_publish_ns, fusion_publish_ns);
}

TEST(TradeFusionCoreTest, PublishesOnlyIncreasingTradeIdsPerSymbol) {
  aquila::market_data::TradeFusionCore fusion(/*max_symbol_id=*/128);

  const aquila::Trade first = MakeTrade(42, 100, 1'000);
  ExpectPublishedDecision(
      fusion.OnTrade(/*source_id=*/0, first, /*fusion_publish_ns=*/2'000),
      first, 2'000, 0);

  EXPECT_FALSE(fusion
                   .OnTrade(/*source_id=*/1, MakeTrade(42, 100, 1'100),
                            /*fusion_publish_ns=*/2'100)
                   .publish);
  EXPECT_FALSE(fusion
                   .OnTrade(/*source_id=*/2, MakeTrade(42, 99, 1'200),
                            /*fusion_publish_ns=*/2'200)
                   .publish);

  const aquila::Trade next = MakeTrade(42, 101, 1'300);
  ExpectPublishedDecision(
      fusion.OnTrade(/*source_id=*/3, next, /*fusion_publish_ns=*/2'300),
      next, 2'300, 3);
}

TEST(TradeFusionCoreTest, MaintainsIndependentStatePerSymbol) {
  aquila::market_data::TradeFusionCore fusion(/*max_symbol_id=*/128);

  const aquila::Trade first_symbol = MakeTrade(1, 10, 3'000);
  const aquila::Trade second_symbol = MakeTrade(2, 5, 3'100);
  ExpectPublishedDecision(fusion.OnTrade(/*source_id=*/0, first_symbol,
                                         /*fusion_publish_ns=*/4'000),
                          first_symbol, 4'000, 0);
  ExpectPublishedDecision(fusion.OnTrade(/*source_id=*/1, second_symbol,
                                         /*fusion_publish_ns=*/4'100),
                          second_symbol, 4'100, 1);

  EXPECT_FALSE(fusion
                   .OnTrade(/*source_id=*/2, MakeTrade(1, 9, 3'200),
                            /*fusion_publish_ns=*/4'200)
                   .publish);
}

TEST(TradeFusionCoreTest, DropsOutOfRangeSymbolsWithUnsetMetadata) {
  aquila::market_data::TradeFusionCore fusion(/*max_symbol_id=*/8);

  const aquila::market_data::TradeFusionDecision invalid =
      fusion.OnTrade(/*source_id=*/0, MakeTrade(9, 1, 5'000),
                     /*fusion_publish_ns=*/6'000);
  EXPECT_FALSE(invalid.publish);
  EXPECT_EQ(invalid.source_id, -1);
  EXPECT_EQ(invalid.symbol_id, -1);
  EXPECT_EQ(invalid.trade_id, 0);
  EXPECT_EQ(invalid.source_local_ns, 0);
  EXPECT_EQ(invalid.fusion_publish_ns, 0);
}

}  // namespace
