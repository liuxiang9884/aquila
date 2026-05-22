#include "monitor/market_data/market_data_store.h"

#include <array>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace aquila::monitor {
namespace {

BookTicker MakeTicker(Exchange exchange, std::int32_t symbol_id,
                      std::int64_t id, double bid_price = 100.0,
                      double ask_price = 101.0) {
  return BookTicker{
      .id = id,
      .symbol_id = symbol_id,
      .exchange = exchange,
      .exchange_ns = 1000 + id,
      .local_ns = 2000 + id,
      .bid_price = bid_price,
      .bid_volume = 1.5,
      .ask_price = ask_price,
      .ask_volume = 2.5,
  };
}

TEST(MarketDataStoreTest, FirstTickerBuildsChangedBatch) {
  constexpr std::array<MarketDataKey, 1> keys = {
      MarketDataKey{.exchange = Exchange::kGate, .symbol_id = 7},
  };
  MarketDataStore store(keys);

  store.OnBookTicker(MakeTicker(Exchange::kGate, 7, 11, 62.85, 62.86));
  const MarketDataBatch batch = store.BuildChangedBatch(123456);

  ASSERT_EQ(batch.row_count, 1);
  EXPECT_EQ(batch.published_ns, 123456);
  EXPECT_EQ(batch.rows[0].exchange, Exchange::kGate);
  EXPECT_EQ(batch.rows[0].symbol_id, 7);
  EXPECT_EQ(batch.rows[0].id, 11);
  EXPECT_EQ(batch.rows[0].exchange_ns, 1011);
  EXPECT_EQ(batch.rows[0].local_ns, 2011);
  EXPECT_DOUBLE_EQ(batch.rows[0].bid_price, 62.85);
  EXPECT_DOUBLE_EQ(batch.rows[0].ask_price, 62.86);
  EXPECT_EQ(batch.drained_count, 1);
  EXPECT_EQ(store.stats().changed_count, 1);
}

TEST(MarketDataStoreTest, SameIdDoesNotRepeatChangedRow) {
  constexpr std::array<MarketDataKey, 1> keys = {
      MarketDataKey{.exchange = Exchange::kGate, .symbol_id = 7},
  };
  MarketDataStore store(keys);

  store.OnBookTicker(MakeTicker(Exchange::kGate, 7, 11));
  ASSERT_EQ(store.BuildChangedBatch(1).row_count, 1);

  store.OnBookTicker(MakeTicker(Exchange::kGate, 7, 11, 99.0, 100.0));
  const MarketDataBatch batch = store.BuildChangedBatch(2);

  EXPECT_EQ(batch.row_count, 0);
  EXPECT_EQ(batch.drained_count, 2);
  EXPECT_EQ(store.stats().changed_count, 1);
}

TEST(MarketDataStoreTest, NewIdUpdatesLatestRow) {
  constexpr std::array<MarketDataKey, 1> keys = {
      MarketDataKey{.exchange = Exchange::kBinance, .symbol_id = 9},
  };
  MarketDataStore store(keys);

  store.OnBookTicker(MakeTicker(Exchange::kBinance, 9, 20, 10.0, 11.0));
  ASSERT_EQ(store.BuildChangedBatch(1).row_count, 1);

  store.OnBookTicker(MakeTicker(Exchange::kBinance, 9, 21, 12.0, 13.0));
  const MarketDataBatch batch = store.BuildChangedBatch(2);

  ASSERT_EQ(batch.row_count, 1);
  EXPECT_EQ(batch.rows[0].id, 21);
  EXPECT_DOUBLE_EQ(batch.rows[0].bid_price, 12.0);
  EXPECT_DOUBLE_EQ(batch.rows[0].ask_price, 13.0);
  EXPECT_EQ(store.stats().changed_count, 2);
}

TEST(MarketDataStoreTest, GateAndBinanceWithSameSymbolIdAreSeparateRows) {
  constexpr std::array<MarketDataKey, 2> keys = {
      MarketDataKey{.exchange = Exchange::kGate, .symbol_id = 7},
      MarketDataKey{.exchange = Exchange::kBinance, .symbol_id = 7},
  };
  MarketDataStore store(keys);

  store.OnBookTicker(MakeTicker(Exchange::kGate, 7, 31, 50.0, 51.0));
  store.OnBookTicker(MakeTicker(Exchange::kBinance, 7, 41, 60.0, 61.0));
  const MarketDataBatch batch = store.BuildChangedBatch(3);

  ASSERT_EQ(batch.row_count, 2);
  EXPECT_EQ(batch.rows[0].exchange, Exchange::kGate);
  EXPECT_EQ(batch.rows[0].id, 31);
  EXPECT_DOUBLE_EQ(batch.rows[0].bid_price, 50.0);
  EXPECT_EQ(batch.rows[1].exchange, Exchange::kBinance);
  EXPECT_EQ(batch.rows[1].id, 41);
  EXPECT_DOUBLE_EQ(batch.rows[1].bid_price, 60.0);
}

TEST(MarketDataStoreTest, UnknownSymbolIsCountedAndIgnored) {
  constexpr std::array<MarketDataKey, 1> keys = {
      MarketDataKey{.exchange = Exchange::kGate, .symbol_id = 7},
  };
  MarketDataStore store(keys);

  store.OnBookTicker(MakeTicker(Exchange::kGate, 8, 1));
  store.OnBookTicker(MakeTicker(Exchange::kBinance, 7, 2));
  const MarketDataBatch batch = store.BuildChangedBatch(4);

  EXPECT_EQ(batch.row_count, 0);
  EXPECT_EQ(batch.drained_count, 2);
  EXPECT_EQ(store.stats().unknown_symbol_count, 2);
  EXPECT_EQ(store.stats().changed_count, 0);
}

TEST(MarketDataStoreTest, BuildBatchClearsChangedFlags) {
  constexpr std::array<MarketDataKey, 1> keys = {
      MarketDataKey{.exchange = Exchange::kGate, .symbol_id = 7},
  };
  MarketDataStore store(keys);

  store.OnBookTicker(MakeTicker(Exchange::kGate, 7, 11));
  ASSERT_EQ(store.BuildChangedBatch(1).row_count, 1);

  const MarketDataBatch batch = store.BuildChangedBatch(2);

  EXPECT_EQ(batch.row_count, 0);
  EXPECT_EQ(batch.drained_count, 1);
}

TEST(MarketDataStoreTest, OverrunAndDroppedBatchStatsAppearInBatch) {
  constexpr std::array<MarketDataKey, 1> keys = {
      MarketDataKey{.exchange = Exchange::kGate, .symbol_id = 7},
  };
  MarketDataStore store(keys);

  store.RecordOverrun(3);
  store.RecordDroppedBatch();
  store.OnBookTicker(MakeTicker(Exchange::kGate, 7, 11));
  const MarketDataBatch batch = store.BuildChangedBatch(5);

  EXPECT_EQ(batch.overrun_count, 3);
  EXPECT_EQ(batch.dropped_batch_count, 1);
  EXPECT_EQ(store.stats().overrun_count, 3);
  EXPECT_EQ(store.stats().dropped_batch_count, 1);
}

}  // namespace
}  // namespace aquila::monitor
