#include "tools/market_data/data_reader_recorder.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/config/data_reader_config.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/realtime_data_reader.h"

namespace aquila::tools::market_data {
namespace {

namespace cfg = aquila::config;
namespace md = aquila::market_data;

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

  std::string shm_name;
};

std::string UniqueShmName(std::string_view suffix) {
  std::string name{"/aquila_data_reader_recorder_test_"};
  name.append(std::to_string(::getpid()));
  name.push_back('_');
  name.append(suffix);
  return name;
}

md::BookTickerShmConfig MakeCreateConfig(std::string_view suffix) {
  return md::BookTickerShmConfig{
      .enabled = true,
      .shm_name = UniqueShmName(suffix),
      .channel_name = "book_ticker_channel",
      .create = true,
      .remove_existing = true,
  };
}

cfg::DataReaderSourceConfig MakeSourceConfig(std::string name,
                                             Exchange exchange,
                                             std::string shm_name) {
  return cfg::DataReaderSourceConfig{
      .name = std::move(name),
      .type = cfg::DataReaderSourceType::kShm,
      .exchange = exchange,
      .feed = cfg::DataReaderFeed::kBookTicker,
      .shm_name = std::move(shm_name),
      .channel_name = "book_ticker_channel",
      .start_position = cfg::DataReaderStartPosition::kLatest,
      .read_mode = cfg::DataReaderReadMode::kDrain,
      .required = true,
  };
}

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
     RealtimeReaderDrainsTwoShmSourcesIntoSingleReplayBinary) {
  const md::BookTickerShmConfig gate_config = MakeCreateConfig("gate");
  const md::BookTickerShmConfig binance_config = MakeCreateConfig("binance");
  ShmCleanup gate_cleanup(gate_config.shm_name);
  ShmCleanup binance_cleanup(binance_config.shm_name);

  md::DataShmPublisher gate_publisher(gate_config);
  md::DataShmPublisher binance_publisher(binance_config);

  cfg::DataReaderConfig config;
  config.name = "recorder_integration_test";
  config.max_events_per_drain = 64;
  config.sources.push_back(MakeSourceConfig("gate_book_ticker", Exchange::kGate,
                                            gate_config.shm_name));
  config.sources.push_back(MakeSourceConfig(
      "binance_book_ticker", Exchange::kBinance, binance_config.shm_name));

  using Reader = md::RealtimeDataReader<md::RealtimeDataReaderDiagnostics>;
  Reader reader(std::move(config));

  const BookTicker gate_first =
      MakeTicker(101, Exchange::kGate, 1'770'000'000'000'000'101,
                 1'770'000'000'000'010'101);
  const BookTicker gate_second =
      MakeTicker(102, Exchange::kGate, 1'770'000'000'000'000'102,
                 1'770'000'000'000'010'102);
  const BookTicker binance_first =
      MakeTicker(201, Exchange::kBinance, 1'770'000'000'000'000'201,
                 1'770'000'000'000'010'201);
  const BookTicker binance_second =
      MakeTicker(202, Exchange::kBinance, 1'770'000'000'000'000'202,
                 1'770'000'000'000'010'202);

  gate_publisher.OnBookTicker(gate_first);
  gate_publisher.OnBookTicker(gate_second);
  binance_publisher.OnBookTicker(binance_first);
  binance_publisher.OnBookTicker(binance_second);

  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      ("aquila_data_reader_recorder_shm_test_" + std::to_string(::getpid()) +
       ".bin");
  std::filesystem::remove(output_path);

  {
    BookTickerBinaryRecorder recorder(output_path,
                                      RecorderWriteMode::kTruncate);
    EXPECT_EQ(reader.Drain(recorder, config.max_events_per_drain), 4U);
    EXPECT_TRUE(recorder.Flush());
    EXPECT_FALSE(recorder.write_error());

    const RecorderStats& stats = recorder.stats();
    EXPECT_EQ(stats.total_records, 4U);
    EXPECT_EQ(stats.RecordsForExchange(Exchange::kGate), 2U);
    EXPECT_EQ(stats.RecordsForExchange(Exchange::kBinance), 2U);

    const md::RealtimeDataReaderStats& reader_stats =
        reader.diagnostics().stats();
    EXPECT_EQ(reader_stats.total_count, 4U);
    ASSERT_EQ(reader_stats.sources.size(), 2U);
    EXPECT_EQ(reader_stats.sources[0].book_ticker_count, 2U);
    EXPECT_EQ(reader_stats.sources[1].book_ticker_count, 2U);
  }

  EXPECT_EQ(std::filesystem::file_size(output_path), 4U * sizeof(BookTicker));
  const std::vector<BookTicker> records = ReadBookTickers(output_path);
  ASSERT_EQ(records.size(), 4U);
  ExpectBookTickerEq(records[0], gate_first);
  ExpectBookTickerEq(records[1], binance_first);
  ExpectBookTickerEq(records[2], gate_second);
  ExpectBookTickerEq(records[3], binance_second);

  std::filesystem::remove(output_path);
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
