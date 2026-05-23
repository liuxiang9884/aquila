#include "tools/market_data/data_reader_recorder.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

namespace aquila::tools::market_data {
namespace {

BookTicker MakeTicker(std::int64_t id, Exchange exchange,
                      std::int64_t exchange_ns,
                      std::int64_t local_ns) noexcept {
  return BookTicker{
      .id = id,
      .symbol_id = 42,
      .exchange = exchange,
      .exchange_ns = exchange_ns,
      .local_ns = local_ns,
      .bid_price = 65'000.0 + static_cast<double>(id),
      .bid_volume = 10.0 + static_cast<double>(id),
      .ask_price = 65'001.0 + static_cast<double>(id),
      .ask_volume = 11.0 + static_cast<double>(id),
  };
}

void ExpectBookTickerEq(const BookTicker& actual, const BookTicker& expected) {
  EXPECT_EQ(actual.id, expected.id);
  EXPECT_EQ(actual.symbol_id, expected.symbol_id);
  EXPECT_EQ(actual.exchange, expected.exchange);
  EXPECT_EQ(actual.exchange_ns, expected.exchange_ns);
  EXPECT_EQ(actual.local_ns, expected.local_ns);
  EXPECT_DOUBLE_EQ(actual.bid_price, expected.bid_price);
  EXPECT_DOUBLE_EQ(actual.bid_volume, expected.bid_volume);
  EXPECT_DOUBLE_EQ(actual.ask_price, expected.ask_price);
  EXPECT_DOUBLE_EQ(actual.ask_volume, expected.ask_volume);
}

std::vector<BookTicker> ReadBookTickers(
    const std::filesystem::path& output_path) {
  const std::uintmax_t size = std::filesystem::file_size(output_path);
  EXPECT_EQ(size % sizeof(BookTicker), 0U);
  std::vector<BookTicker> records(size / sizeof(BookTicker));

  std::ifstream input(output_path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  if (!records.empty()) {
    input.read(
        reinterpret_cast<char*>(records.data()),
        static_cast<std::streamsize>(records.size() * sizeof(BookTicker)));
    EXPECT_TRUE(input.good());
  }
  return records;
}

TEST(DataReaderRecorderTest,
     WritesBareBookTickerRecordsAndTracksRunStatistics) {
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      "aquila_data_reader_recorder_test.bin";
  std::filesystem::remove(output_path);

  const BookTicker gate =
      MakeTicker(10, Exchange::kGate, 1'770'000'000'000'000'100,
                 1'770'000'000'000'010'100);
  const BookTicker binance =
      MakeTicker(20, Exchange::kBinance, 1'770'000'000'000'000'200,
                 1'770'000'000'000'010'200);

  {
    BookTickerBinaryRecorder recorder(output_path,
                                      RecorderWriteMode::kTruncate);
    recorder.OnBookTicker(gate);
    recorder.OnBookTicker(binance);

    const RecorderStats& stats = recorder.stats();
    EXPECT_EQ(stats.total_records, 2U);
    EXPECT_EQ(stats.RecordsForExchange(Exchange::kGate), 1U);
    EXPECT_EQ(stats.RecordsForExchange(Exchange::kBinance), 1U);
    EXPECT_EQ(stats.RecordsForExchange(Exchange::kOkx), 0U);
    ASSERT_TRUE(stats.first_exchange_ns.has_value());
    ASSERT_TRUE(stats.first_local_ns.has_value());
    ASSERT_TRUE(stats.last_exchange_ns.has_value());
    ASSERT_TRUE(stats.last_local_ns.has_value());
    EXPECT_EQ(*stats.first_exchange_ns, gate.exchange_ns);
    EXPECT_EQ(*stats.first_local_ns, gate.local_ns);
    EXPECT_EQ(*stats.last_exchange_ns, binance.exchange_ns);
    EXPECT_EQ(*stats.last_local_ns, binance.local_ns);
    EXPECT_FALSE(recorder.write_error());
  }

  EXPECT_EQ(std::filesystem::file_size(output_path), 2U * sizeof(BookTicker));
  const std::vector<BookTicker> records = ReadBookTickers(output_path);
  ASSERT_EQ(records.size(), 2U);
  ExpectBookTickerEq(records[0], gate);
  ExpectBookTickerEq(records[1], binance);

  std::filesystem::remove(output_path);
}

TEST(DataReaderRecorderTest, AppendModePreservesExistingRecords) {
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      "aquila_data_reader_recorder_append_test.bin";
  std::filesystem::remove(output_path);

  const BookTicker first = MakeTicker(
      1, Exchange::kGate, 1'770'000'000'000'000'001, 1'770'000'000'000'010'001);
  const BookTicker second =
      MakeTicker(2, Exchange::kBinance, 1'770'000'000'000'000'002,
                 1'770'000'000'000'010'002);

  {
    BookTickerBinaryRecorder recorder(output_path,
                                      RecorderWriteMode::kTruncate);
    recorder.OnBookTicker(first);
    EXPECT_FALSE(recorder.write_error());
  }
  {
    BookTickerBinaryRecorder recorder(output_path, RecorderWriteMode::kAppend);
    recorder.OnBookTicker(second);
    EXPECT_EQ(recorder.stats().total_records, 1U);
    EXPECT_FALSE(recorder.write_error());
  }

  const std::vector<BookTicker> records = ReadBookTickers(output_path);
  ASSERT_EQ(records.size(), 2U);
  ExpectBookTickerEq(records[0], first);
  ExpectBookTickerEq(records[1], second);

  std::filesystem::remove(output_path);
}

}  // namespace
}  // namespace aquila::tools::market_data
