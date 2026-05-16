#include "tools/lead_lag/signal_csv_writer.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/signal.h"

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
                        /*reduce_only=*/false));
  writer.Write(Ticker(/*id=*/8, /*exchange_ns=*/1776211200000002000LL,
                      /*local_ns=*/1776211200000003000LL),
               Decision(strategy::leadlag::SignalAction::kCloseLong,
                        OrderSide::kSell, /*price=*/2.2,
                        /*reduce_only=*/true));
  writer.Close();

  EXPECT_EQ(
      ReadFile(output_path),
      "ticker_id,symbol_id,exchange_ns,local_ns,action,side,price,"
      "reduce_only\n"
      "7,3,1776211200000000000,1776211200000001000,kOpenLong,kBuy,"
      "2.12345678901,false\n"
      "8,3,1776211200000002000,1776211200000003000,kCloseLong,kSell,2.2,"
      "true\n");
}

}  // namespace
}  // namespace aquila::tools::lead_lag
