#include "tools/lead_lag/signal_csv_writer.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/signal.h"
#include "strategy/lead_lag/strategy.h"

namespace aquila::tools::lead_lag {
namespace {

void EnsureLoggingStarted() {
  static const bool started = [] {
    nova::LogConfig config;
    config.set_console_sink_name("");
    config.set_file_sink_name(
        (std::filesystem::temp_directory_path() /
         "aquila_signal_csv_writer_test.log")
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

BookTicker Ticker(std::int64_t id, std::int64_t exchange_ns,
                  std::int64_t local_ns) {
  return BookTicker{
      .id = id,
      .symbol_id = 3,
      .exchange = Exchange::kGate,
      .exchange_ns = exchange_ns,
      .local_ns = local_ns,
      .bid_price = 2.0,
      .bid_volume = 10.0,
      .ask_price = 2.1,
      .ask_volume = 11.0,
  };
}

strategy::leadlag::SignalDecision Decision(
    strategy::leadlag::SignalAction action, OrderSide side, double price,
    bool reduce_only) {
  return strategy::leadlag::SignalDecision{
      .triggered = true,
      .action = action,
      .reject_reason = strategy::leadlag::SignalRejectReason::kNone,
      .intent =
          strategy::leadlag::OrderIntent{
              .action = action,
              .exchange = Exchange::kGate,
              .symbol_id = 3,
              .side = side,
              .price = price,
              .reduce_only = reduce_only,
          },
  };
}

strategy::leadlag::SignalDiagnostics Diagnostics() {
  return strategy::leadlag::SignalDiagnostics{
      .event_ns = 1776211200000000000LL,
      .role = strategy::leadlag::PairRole::kLead,
      .price_changed = true,
      .lead_raw =
          strategy::leadlag::QuoteSnapshot{
              .event_ns = 1776211199999999000LL,
              .bid_price = 2.1,
              .ask_price = 2.2,
          },
      .lead_drifted =
          strategy::leadlag::QuoteSnapshot{
              .event_ns = 1776211199999999000LL,
              .bid_price = 2.11,
              .ask_price = 2.21,
          },
      .lag =
          strategy::leadlag::QuoteSnapshot{
              .event_ns = 1776211200000000000LL,
              .bid_price = 2.0,
              .ask_price = 2.01,
          },
      .alignment =
          strategy::leadlag::AlignmentSnapshot{
              .drift_ready = true,
              .drift_mean = 1.00476190476,
              .drift_deviation = 0.0003,
          },
      .threshold =
          strategy::leadlag::ThresholdSnapshot{
              .initialized = true,
              .up_entry = 0.004,
              .down_entry = -0.004,
              .up_exit = 0.001,
              .down_exit = -0.001,
          },
      .recorder =
          strategy::leadlag::RecorderSnapshot{
              .lead_noise = 0.00011,
              .lag_noise = 0.00022,
              .lag_spread_mean = 0.01,
          },
      .active_group_count = 1,
      .position_direction = strategy::leadlag::PositionDirection::kLong,
      .trailing_price = 2.05,
  };
}

TEST(SignalCsvWriterTest, WritesHeaderAndRowsThroughQuillCsvWriter) {
  EnsureLoggingStarted();
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      "aquila_signal_csv_writer_test.csv";
  std::filesystem::remove(output_path);

  SignalCsvWriter writer;
  std::string error;
  ASSERT_TRUE(writer.Open(output_path, &error)) << error;
  writer.Write(Ticker(/*id=*/7, /*exchange_ns=*/1776211200000000000LL,
                      /*local_ns=*/1776211200000001000LL),
               Decision(strategy::leadlag::SignalAction::kOpenLong,
                        OrderSide::kBuy, /*price=*/2.1234567890123,
                        /*reduce_only=*/false),
               Diagnostics());
  writer.Write(Ticker(/*id=*/8, /*exchange_ns=*/1776211200000002000LL,
                      /*local_ns=*/1776211200000003000LL),
               Decision(strategy::leadlag::SignalAction::kCloseLong,
                        OrderSide::kSell, /*price=*/2.2,
                        /*reduce_only=*/true),
               Diagnostics());
  writer.Close();

  EXPECT_EQ(
      ReadFile(output_path),
      "ticker_id,symbol_id,exchange,role,exchange_ns,local_ns,event_ns,"
      "price_changed,action,side,price,reduce_only,lead_raw_event_ns,"
      "lead_raw_bid,lead_raw_ask,lead_drifted_event_ns,lead_drifted_bid,"
      "lead_drifted_ask,lag_event_ns,lag_bid,lag_ask,drift_mean,"
      "drift_ready,drift_deviation,up_entry,down_entry,up_exit,down_exit,"
      "lag_spread_mean,lead_noise,lag_noise,active_group_count,"
      "position_direction,trailing_price\n"
      "7,3,kGate,kLead,1776211200000000000,1776211200000001000,"
      "1776211200000000000,true,kOpenLong,kBuy,2.12345678901,false,"
      "1776211199999999000,2.1,2.2,1776211199999999000,2.11,2.21,"
      "1776211200000000000,2,2.01,1.00476190476,true,0.0003,0.004,"
      "-0.004,0.001,-0.001,0.01,0.00011,0.00022,1,kLong,2.05\n"
      "8,3,kGate,kLead,1776211200000002000,1776211200000003000,"
      "1776211200000000000,true,kCloseLong,kSell,2.2,true,"
      "1776211199999999000,2.1,2.2,1776211199999999000,2.11,2.21,"
      "1776211200000000000,2,2.01,1.00476190476,true,0.0003,0.004,"
      "-0.004,0.001,-0.001,0.01,0.00011,0.00022,1,kLong,2.05\n");
}

}  // namespace
}  // namespace aquila::tools::lead_lag
