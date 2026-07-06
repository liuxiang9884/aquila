#include "evaluation/market_data/book_ticker_replay_benchmark_support.h"

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "core/market_data/market_data_binary_format.h"
#include "core/market_data/types.h"

namespace aquila::market_data::evaluation {
namespace {

std::filesystem::path TempPath(const std::string& suffix) {
  return std::filesystem::path{"/home/liuxiang/tmp"} /
         ("aquila_book_ticker_replay_benchmark_support_test_" +
          std::to_string(::getpid()) + "_" + suffix);
}

BookTicker MakeTicker(std::int64_t id, std::int64_t exchange_ns) {
  return BookTicker{
      .id = id,
      .symbol_id = 42,
      .exchange = Exchange::kGate,
      .exchange_ns = exchange_ns,
      .local_ns = exchange_ns + 1'000,
      .bid_price = 65'000.0 + static_cast<double>(id),
      .bid_volume = 10.0,
      .ask_price = 65'001.0 + static_cast<double>(id),
      .ask_volume = 11.0,
  };
}

void WriteTicker(std::ofstream& output, const BookTicker& ticker) {
  output.write(reinterpret_cast<const char*>(&ticker), sizeof(ticker));
}

void WriteTypedHeader(std::ofstream& output) {
  ASSERT_TRUE(aquila::market_data::WriteMarketDataBinaryHeader(
      output, aquila::config::DataReaderFeed::kBookTicker));
}

TEST(BookTickerReplayBenchmarkSupportTest, LoadsBookTickerDump) {
  const std::filesystem::path path = TempPath("valid.bin");
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    WriteTypedHeader(output);
    WriteTicker(output, MakeTicker(1, 1'000));
    WriteTicker(output, MakeTicker(2, 1'300));
  }

  const auto result = LoadBookTickerDump(path, 0);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.size(), 2U);
  EXPECT_EQ(result.value[0].id, 1);
  EXPECT_EQ(result.value[1].exchange_ns, 1'300);
  std::filesystem::remove(path);
}

TEST(BookTickerReplayBenchmarkSupportTest, LimitsLoadedRecords) {
  const std::filesystem::path path = TempPath("limited.bin");
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    WriteTypedHeader(output);
    WriteTicker(output, MakeTicker(1, 1'000));
    WriteTicker(output, MakeTicker(2, 1'300));
  }

  const auto result = LoadBookTickerDump(path, 1);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.size(), 1U);
  EXPECT_EQ(result.value[0].id, 1);
  std::filesystem::remove(path);
}

TEST(BookTickerReplayBenchmarkSupportTest, HeaderOnlyDumpLoadsAsEmpty) {
  const std::filesystem::path path = TempPath("empty.bin");
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    WriteTypedHeader(output);
  }

  const auto result = LoadBookTickerDump(path, 0);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.empty());
  std::filesystem::remove(path);
}

TEST(BookTickerReplayBenchmarkSupportTest, RejectsRawBookTickerDump) {
  const std::filesystem::path path = TempPath("raw.bin");
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    WriteTicker(output, MakeTicker(1, 1'000));
  }

  const auto result = LoadBookTickerDump(path, 0);

  EXPECT_FALSE(result.ok);
  std::filesystem::remove(path);
}

TEST(BookTickerReplayBenchmarkSupportTest, RejectsPartialRecordDump) {
  const std::filesystem::path path = TempPath("partial.bin");
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    WriteTypedHeader(output);
    const char byte = 7;
    output.write(&byte, 1);
  }

  const auto result = LoadBookTickerDump(path, 0);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("payload size"), std::string::npos);
  std::filesystem::remove(path);
}

TEST(BookTickerReplayBenchmarkSupportTest, ComputesScaledReplayDelay) {
  EXPECT_EQ(ReplayDelayNs(1'000, 1'100, 16.0), 6);
  EXPECT_EQ(ReplayDelayNs(1'000, 1'100, 4.0), 25);
  EXPECT_EQ(ReplayDelayNs(1'100, 1'100, 16.0), 0);
  EXPECT_EQ(ReplayDelayNs(1'200, 1'100, 16.0), 0);
}

TEST(BookTickerReplayBenchmarkSupportTest, ComputesLatencySummary) {
  const LatencySummary summary = SummarizeLatencies({10, 20, 30, 40});

  EXPECT_EQ(summary.count, 4U);
  EXPECT_EQ(summary.min_ns, 10);
  EXPECT_EQ(summary.p50_ns, 20);
  EXPECT_EQ(summary.p95_ns, 40);
  EXPECT_EQ(summary.p99_ns, 40);
  EXPECT_EQ(summary.p999_ns, 40);
  EXPECT_EQ(summary.max_ns, 40);
}

}  // namespace
}  // namespace aquila::market_data::evaluation
