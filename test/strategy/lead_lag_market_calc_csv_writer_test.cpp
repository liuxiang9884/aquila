#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/market_calc_csv_writer.h"

namespace aquila::strategy::leadlag {
namespace {

void EnsureLoggingStarted() {
  static const bool started = [] {
    nova::LogConfig config;
    config.set_console_sink_name("");
    config.set_file_sink_name(
        (std::filesystem::temp_directory_path() /
         "aquila_lead_lag_market_calc_csv_writer_test.log")
            .string());
    nova::InitializeLogging(config);
    return true;
  }();
  (void)started;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

double Nan() noexcept {
  return std::numeric_limits<double>::quiet_NaN();
}

MarketCalcRow LeadRow() {
  return MarketCalcRow{
      .row_index = 1,
      .role = PairRole::kLead,
      .symbol = "BTC_USDT",
      .symbol_id = 3,
      .book_ticker_id = 100,
      .exchange = Exchange::kBinance,
      .exchange_ns = 1776211200000000000LL,
      .local_ns = 1776211200000001000LL,
      .event_ns = 1776211200000000000LL,
      .price_changed = true,
      .both_sides_valid = true,
      .active = true,
      .lead_bid = 112.0,
      .lead_ask = 113.0,
      .lag_bid = 101.57,
      .lag_ask = 102.02,
      .drift_mean = 1.00476190476,
      .drift_std_ema = 0.0003,
      .drifted_lead_bid = 112.533333333,
      .drifted_lead_ask = 113.538095238,
      .up_entry = 0.02,
      .down_entry = -0.02,
      .up_exit = 0.005,
      .down_exit = -0.005,
      .lead_noise = 0.00011,
      .lag_noise = 0.00022,
      .lag_spread_mean = 0.41,
      .long_lead_move = 0.12,
      .long_price_diff = 0.103704960,
      .long_lag_part_ratio = 1.0,
      .long_target_space = 0.0981866301,
      .long_required_edge = 0.00133,
      .short_lead_move = Nan(),
      .short_price_diff = Nan(),
      .short_lag_part_ratio = Nan(),
      .short_target_space = Nan(),
      .short_required_edge = Nan(),
      .lag_spread_pct = 0.0044191481,
  };
}

MarketCalcRow LagRow() {
  MarketCalcRow row = LeadRow();
  row.row_index = 2;
  row.role = PairRole::kLag;
  row.book_ticker_id = 101;
  row.exchange = Exchange::kGate;
  row.exchange_ns = 1776211200000002000LL;
  row.local_ns = 1776211200000003000LL;
  row.event_ns = 1776211200000002000LL;
  row.lag_spread = 0.45;
  row.lag_spread_buffer = 0.04;
  row.lag_spread_pct = 0.0044191481;
  return row;
}

TEST(LeadLagMarketCalcCsvWriterTest,
     WritesLeadAndLagRowsWithStableTextFormatting) {
  EnsureLoggingStarted();
  const std::filesystem::path output_dir =
      std::filesystem::temp_directory_path() /
      "aquila_lead_lag_market_calc_csv_writer_test";
  std::filesystem::remove_all(output_dir);

  MarketCalcCsvWriter writer;
  std::string error;
  ASSERT_TRUE(writer.Open(output_dir, &error)) << error;
  writer.Write(LeadRow());
  writer.Write(LagRow());
  writer.Close();

  EXPECT_EQ(
      ReadFile(output_dir / "lead_calc.csv"),
      "row_index,role,symbol,symbol_id,book_ticker_id,exchange,exchange_ns,"
      "local_ns,event_ns,price_changed,both_sides_valid,active,lead_bid,"
      "lead_ask,lag_bid,lag_ask,drift_mean,drift_std_ema,drifted_lead_bid,"
      "drifted_lead_ask,up_entry,down_entry,up_exit,down_exit,lead_noise,"
      "lag_noise,lag_spread_mean,long_lead_move,long_price_diff,"
      "long_lag_part_ratio,long_target_space,long_required_edge,"
      "short_lead_move,short_price_diff,short_lag_part_ratio,"
      "short_target_space,short_required_edge,lag_spread_pct\n"
      "1,kLead,BTC_USDT,3,100,kBinance,1776211200000000000,"
      "1776211200000001000,1776211200000000000,true,true,true,112,113,"
      "101.57,102.02,1.00476190476,0.0003,112.533333333,113.538095238,"
      "0.02,-0.02,0.005,-0.005,0.00011,0.00022,0.41,0.12,0.10370496,1,"
      "0.0981866301,0.00133,nan,nan,nan,nan,nan,0.0044191481\n");
  EXPECT_EQ(
      ReadFile(output_dir / "lag_calc.csv"),
      "row_index,role,symbol,symbol_id,book_ticker_id,exchange,exchange_ns,"
      "local_ns,event_ns,price_changed,both_sides_valid,active,lead_bid,"
      "lead_ask,lag_bid,lag_ask,drift_mean,drift_std_ema,lag_spread,"
      "lag_spread_mean,lag_spread_buffer,lag_spread_pct,lag_noise\n"
      "2,kLag,BTC_USDT,3,101,kGate,1776211200000002000,"
      "1776211200000003000,1776211200000002000,true,true,true,112,113,"
      "101.57,102.02,1.00476190476,0.0003,0.45,0.41,0.04,0.0044191481,"
      "0.00022\n");
}

}  // namespace
}  // namespace aquila::strategy::leadlag
