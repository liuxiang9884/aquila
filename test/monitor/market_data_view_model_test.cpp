#include "monitor/model/market_data_view_model.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "monitor/demo/symbol_workbench_demo_data.h"
#include "monitor/market_data/market_data_update.h"

namespace aquila::monitor {
namespace {

[[nodiscard]] std::int64_t NsAtTime(int hour, int minute, int second,
                                    int millisecond) {
  constexpr std::int64_t kNsPerMillisecond = 1'000'000;
  constexpr std::int64_t kNsPerSecond = 1'000'000'000;
  return (((static_cast<std::int64_t>(hour) * 60 + minute) * 60 + second) *
          kNsPerSecond) +
         static_cast<std::int64_t>(millisecond) * kNsPerMillisecond;
}

[[nodiscard]] std::array<config::InstrumentInfo, 2> ZecInstruments() {
  return {
      config::InstrumentInfo{
          .symbol_id = 6,
          .exchange = Exchange::kGate,
          .symbol = "ZEC_USDT",
          .exchange_symbol = "ZEC_USDT",
      },
      config::InstrumentInfo{
          .symbol_id = 6,
          .exchange = Exchange::kBinance,
          .symbol = "ZEC_USDT",
          .exchange_symbol = "ZECUSDT",
      },
  };
}

[[nodiscard]] MarketDataBatch ZecBatch() {
  MarketDataBatch batch{};
  batch.row_count = 2;
  batch.rows[0] = MarketDataRowUpdate{
      .exchange = Exchange::kGate,
      .symbol_id = 6,
      .id = 9841,
      .exchange_ns = NsAtTime(12, 34, 56, 700),
      .local_ns = NsAtTime(12, 34, 56, 789),
      .bid_price = 62.85,
      .bid_volume = 18.4,
      .ask_price = 62.86,
      .ask_volume = 9.7,
  };
  batch.rows[1] = MarketDataRowUpdate{
      .exchange = Exchange::kBinance,
      .symbol_id = 6,
      .id = 4427,
      .exchange_ns = NsAtTime(12, 34, 56, 701),
      .local_ns = NsAtTime(12, 34, 56, 790),
      .bid_price = 62.84,
      .bid_volume = 31.2,
      .ask_price = 62.87,
      .ask_volume = 24.1,
  };
  return batch;
}

TEST(MarketDataViewModelTest, AppliesGateAndBinanceRowsForSelectedZec) {
  const auto instruments = ZecInstruments();
  const AccountMonitorSnapshot snapshot = DemoAccountMonitorSnapshot();
  MarketDataViewModel model(snapshot.symbols, instruments,
                            snapshot.selected_symbol);

  model.ApplyBatch(ZecBatch());

  const std::span<const MarketDataRow> rows = model.SelectedRows();
  ASSERT_EQ(rows.size(), 2U);

  EXPECT_EQ(rows[0].exchange, "Gate");
  EXPECT_EQ(rows[0].exchange_symbol, "ZEC_USDT");
  EXPECT_EQ(rows[0].market_data_id, "9841");
  EXPECT_TRUE(rows[0].has_data);
  EXPECT_FALSE(rows[0].has_last_price);
  EXPECT_DOUBLE_EQ(rows[0].bid_price, 62.85);
  EXPECT_DOUBLE_EQ(rows[0].bid_volume, 18.4);
  EXPECT_DOUBLE_EQ(rows[0].ask_price, 62.86);
  EXPECT_DOUBLE_EQ(rows[0].ask_volume, 9.7);
  EXPECT_FALSE(rows[0].has_volume);
  EXPECT_FALSE(rows[0].has_turnover);
  EXPECT_EQ(rows[0].updated_time, "12:34:56.789");

  EXPECT_EQ(rows[1].exchange, "Binance");
  EXPECT_EQ(rows[1].exchange_symbol, "ZECUSDT");
  EXPECT_EQ(rows[1].market_data_id, "4427");
  EXPECT_TRUE(rows[1].has_data);
  EXPECT_FALSE(rows[1].has_last_price);
  EXPECT_DOUBLE_EQ(rows[1].bid_price, 62.84);
  EXPECT_DOUBLE_EQ(rows[1].bid_volume, 31.2);
  EXPECT_DOUBLE_EQ(rows[1].ask_price, 62.87);
  EXPECT_DOUBLE_EQ(rows[1].ask_volume, 24.1);
  EXPECT_FALSE(rows[1].has_volume);
  EXPECT_FALSE(rows[1].has_turnover);
  EXPECT_EQ(rows[1].updated_time, "12:34:56.790");
}

TEST(MarketDataViewModelTest, DuplicateIdBatchKeepsSelectedSymbolsCorrect) {
  const auto instruments = ZecInstruments();
  const AccountMonitorSnapshot snapshot = DemoAccountMonitorSnapshot();
  MarketDataViewModel model(snapshot.symbols, instruments,
                            snapshot.selected_symbol);

  model.ApplyBatch(ZecBatch());
  model.ApplyBatch(ZecBatch());

  const std::span<const MarketDataRow> rows = model.SelectedRows();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].exchange, "Gate");
  EXPECT_EQ(rows[0].exchange_symbol, "ZEC_USDT");
  EXPECT_EQ(rows[0].market_data_id, "9841");
  EXPECT_EQ(rows[1].exchange, "Binance");
  EXPECT_EQ(rows[1].exchange_symbol, "ZECUSDT");
  EXPECT_EQ(rows[1].market_data_id, "4427");
}

}  // namespace
}  // namespace aquila::monitor
