#include "tools/tardis/book_ticker_binary_converter.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <gtest/gtest.h>

namespace aquila::tools::tardis {
namespace {

BookTickerCsvSource BinanceSource() {
  return BookTickerCsvSource{
      .exchange = Exchange::kBinance,
      .csv_exchange = "binance-futures",
      .csv_symbol = "ORDIUSDT",
      .symbol_id = 3,
  };
}

BookTickerCsvSource GateSource() {
  return BookTickerCsvSource{
      .exchange = Exchange::kGate,
      .csv_exchange = "gate-io-futures",
      .csv_symbol = "ORDI_USDT",
      .symbol_id = 3,
  };
}

BookTicker MakeTicker(std::int64_t id, Exchange exchange,
                      std::int64_t exchange_ns) {
  return BookTicker{
      .id = id,
      .symbol_id = 3,
      .exchange = exchange,
      .exchange_ns = exchange_ns,
      .local_ns = exchange_ns + 1000,
      .bid_price = 2.0,
      .bid_volume = 12.0,
      .ask_price = 2.1,
      .ask_volume = 11.0,
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

TEST(BookTickerBinaryConverterTest, ReadsTardisCsvFieldsIntoBookTickerStruct) {
  std::istringstream input{
      "exchange,symbol,timestamp,local_timestamp,ask_amount,ask_price,"
      "bid_price,bid_amount\n"
      "binance-futures,ORDIUSDT,1776211200018000,1776211200020423,"
      "378.5,2.441,2.44,1598.3\n"};

  const std::vector<ParsedBookTickerRecord> records =
      ReadBookTickerCsv(input, BinanceSource());

  ASSERT_EQ(records.size(), 1);
  const BookTicker& ticker = records.front().ticker;
  EXPECT_EQ(ticker.id, 0);
  EXPECT_EQ(ticker.symbol_id, 3);
  EXPECT_EQ(ticker.exchange, Exchange::kBinance);
  EXPECT_EQ(ticker.exchange_ns, 1776211200018000000LL);
  EXPECT_EQ(ticker.local_ns, 1776211200020423000LL);
  EXPECT_DOUBLE_EQ(ticker.ask_volume, 378.5);
  EXPECT_DOUBLE_EQ(ticker.ask_price, 2.441);
  EXPECT_DOUBLE_EQ(ticker.bid_price, 2.44);
  EXPECT_DOUBLE_EQ(ticker.bid_volume, 1598.3);
}

TEST(BookTickerBinaryConverterTest,
     MergesRecordsByExchangeTimestampAndReassignsDailyIds) {
  std::istringstream binance_input{
      "exchange,symbol,timestamp,local_timestamp,ask_amount,ask_price,"
      "bid_price,bid_amount\n"
      "binance-futures,ORDIUSDT,3000,3001,11,2.003,2.002,12\n"};
  std::istringstream gate_input{
      "exchange,symbol,timestamp,local_timestamp,ask_amount,ask_price,"
      "bid_price,bid_amount\n"
      "gate-io-futures,ORDI_USDT,2000,2002,21,2.001,2.000,22\n"};

  const std::vector<ParsedBookTickerRecord> binance_records =
      ReadBookTickerCsv(binance_input, BinanceSource());
  const std::vector<ParsedBookTickerRecord> gate_records =
      ReadBookTickerCsv(gate_input, GateSource());
  const std::vector<BookTicker> merged =
      MergeBookTickerRecords({binance_records, gate_records});

  ASSERT_EQ(merged.size(), 2);
  EXPECT_EQ(merged[0].id, 0);
  EXPECT_EQ(merged[0].exchange, Exchange::kGate);
  EXPECT_EQ(merged[0].exchange_ns, 2000000);
  EXPECT_DOUBLE_EQ(merged[0].bid_price, 2.000);
  EXPECT_EQ(merged[1].id, 1);
  EXPECT_EQ(merged[1].exchange, Exchange::kBinance);
  EXPECT_EQ(merged[1].exchange_ns, 3000000);
  EXPECT_DOUBLE_EQ(merged[1].ask_price, 2.003);
}

TEST(BookTickerBinaryConverterTest, WritesBareBookTickerRecordsWithoutHeader) {
  std::vector<BookTicker> records;
  records.push_back(BookTicker{
      .id = 0,
      .symbol_id = 3,
      .exchange = Exchange::kGate,
      .exchange_ns = 2000000,
      .local_ns = 2002000,
      .bid_price = 2.000,
      .bid_volume = 22.0,
      .ask_price = 2.001,
      .ask_volume = 21.0,
  });
  records.push_back(BookTicker{
      .id = 1,
      .symbol_id = 3,
      .exchange = Exchange::kBinance,
      .exchange_ns = 3000000,
      .local_ns = 3001000,
      .bid_price = 2.002,
      .bid_volume = 12.0,
      .ask_price = 2.003,
      .ask_volume = 11.0,
  });

  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      "aquila_book_ticker_binary_converter_test.bin";
  std::filesystem::remove(output_path);

  WriteBookTickerBinaryFile(output_path, records);

  EXPECT_EQ(std::filesystem::file_size(output_path),
            records.size() * sizeof(BookTicker));

  std::ifstream raw_input(output_path, std::ios::binary);
  ASSERT_TRUE(raw_input.is_open());
  BookTicker first{};
  raw_input.read(reinterpret_cast<char*>(&first), sizeof(first));
  ASSERT_TRUE(raw_input.good());
  EXPECT_EQ(first.id, records.front().id);
  EXPECT_EQ(first.exchange, records.front().exchange);
  EXPECT_EQ(first.exchange_ns, records.front().exchange_ns);
  EXPECT_DOUBLE_EQ(first.ask_volume, records.front().ask_volume);

  const std::vector<BookTicker> loaded = ReadBookTickerBinaryFile(output_path);
  ASSERT_EQ(loaded.size(), records.size());
  for (std::size_t i = 0; i < loaded.size(); ++i) {
    EXPECT_EQ(loaded[i].id, records[i].id);
    EXPECT_EQ(loaded[i].symbol_id, records[i].symbol_id);
    EXPECT_EQ(loaded[i].exchange, records[i].exchange);
    EXPECT_EQ(loaded[i].exchange_ns, records[i].exchange_ns);
    EXPECT_EQ(loaded[i].local_ns, records[i].local_ns);
    EXPECT_DOUBLE_EQ(loaded[i].bid_price, records[i].bid_price);
    EXPECT_DOUBLE_EQ(loaded[i].bid_volume, records[i].bid_volume);
    EXPECT_DOUBLE_EQ(loaded[i].ask_price, records[i].ask_price);
    EXPECT_DOUBLE_EQ(loaded[i].ask_volume, records[i].ask_volume);
  }

  std::filesystem::remove(output_path);
}

TEST(BookTickerBinaryConverterTest,
     StreamingWriterMergesStreamsAndReportsInputCounts) {
  std::istringstream binance_input{
      "exchange,symbol,timestamp,local_timestamp,ask_amount,ask_price,"
      "bid_price,bid_amount\n"
      "binance-futures,ORDIUSDT,1000,1001,11,2.101,2.100,12\n"
      "binance-futures,ORDIUSDT,3000,3001,31,2.301,2.300,32\n"};
  std::istringstream gate_input{
      "exchange,symbol,timestamp,local_timestamp,ask_amount,ask_price,"
      "bid_price,bid_amount\n"
      "gate-io-futures,ORDI_USDT,2000,2002,21,2.201,2.200,22\n"};

  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      "aquila_book_ticker_streaming_writer_test.bin";
  std::filesystem::remove(output_path);

  const std::vector<BookTickerCsvInput> inputs{
      BookTickerCsvInput{.input = &binance_input, .source = BinanceSource()},
      BookTickerCsvInput{.input = &gate_input, .source = GateSource()},
  };
  const BookTickerBinaryWriteStats stats =
      WriteMergedBookTickerCsvStreams(output_path, inputs);

  EXPECT_EQ(stats.records_written, 3);
  ASSERT_EQ(stats.records_by_input.size(), 2);
  EXPECT_EQ(stats.records_by_input[0], 2);
  EXPECT_EQ(stats.records_by_input[1], 1);
  EXPECT_EQ(stats.first_exchange_ns, 1000000);
  EXPECT_EQ(stats.last_exchange_ns, 3000000);

  const std::vector<BookTicker> loaded = ReadBookTickerBinaryFile(output_path);
  ASSERT_EQ(loaded.size(), 3);
  EXPECT_EQ(loaded[0].id, 0);
  EXPECT_EQ(loaded[0].exchange, Exchange::kBinance);
  EXPECT_EQ(loaded[0].exchange_ns, 1000000);
  EXPECT_EQ(loaded[1].id, 1);
  EXPECT_EQ(loaded[1].exchange, Exchange::kGate);
  EXPECT_EQ(loaded[1].exchange_ns, 2000000);
  EXPECT_EQ(loaded[2].id, 2);
  EXPECT_EQ(loaded[2].exchange, Exchange::kBinance);
  EXPECT_EQ(loaded[2].exchange_ns, 3000000);

  std::filesystem::remove(output_path);
}

TEST(BookTickerBinaryConverterTest,
     ScopedOutputReplacementKeepsFinalFileUntilCommit) {
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      "aquila_book_ticker_atomic_replacement_test.bin";
  const std::filesystem::path temp_path =
      std::filesystem::path{output_path.string() + ".tmp.test"};
  std::filesystem::remove(output_path);
  std::filesystem::remove(temp_path);

  const std::vector<BookTicker> original{
      MakeTicker(0, Exchange::kGate, 2000000)};
  const std::vector<BookTicker> replacement{
      MakeTicker(0, Exchange::kBinance, 3000000),
      MakeTicker(1, Exchange::kGate, 4000000)};
  WriteBookTickerBinaryFile(output_path, original);

  {
    ScopedOutputFileReplacement scoped_output(output_path, ".tmp.test");
    WriteBookTickerBinaryFile(scoped_output.temp_path(), replacement);
  }
  EXPECT_FALSE(std::filesystem::exists(temp_path));
  const std::vector<BookTicker> after_rollback =
      ReadBookTickerBinaryFile(output_path);
  ASSERT_EQ(after_rollback.size(), original.size());
  ExpectBookTickerEq(after_rollback[0], original[0]);

  {
    ScopedOutputFileReplacement scoped_output(output_path, ".tmp.test");
    WriteBookTickerBinaryFile(scoped_output.temp_path(), replacement);
    scoped_output.Commit();
  }
  EXPECT_FALSE(std::filesystem::exists(temp_path));
  const std::vector<BookTicker> after_commit =
      ReadBookTickerBinaryFile(output_path);
  ASSERT_EQ(after_commit.size(), replacement.size());
  ExpectBookTickerEq(after_commit[0], replacement[0]);
  ExpectBookTickerEq(after_commit[1], replacement[1]);

  std::filesystem::remove(output_path);
}

TEST(BookTickerBinaryConverterTest, RejectsUnexpectedCsvSymbol) {
  std::istringstream input{
      "exchange,symbol,timestamp,local_timestamp,ask_amount,ask_price,"
      "bid_price,bid_amount\n"
      "binance-futures,ORDIUSDC,1776211200018000,1776211200020423,"
      "378.5,2.441,2.44,1598.3\n"};

  EXPECT_THROW(static_cast<void>(ReadBookTickerCsv(input, BinanceSource())),
               std::runtime_error);
}

}  // namespace
}  // namespace aquila::tools::tardis
