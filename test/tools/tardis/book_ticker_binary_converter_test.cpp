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
