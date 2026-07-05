#include "tools/market_data/data_reader_recorder.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/realtime_data_reader.h"
#include "tools/market_data/data_reader_recorder_config.h"

namespace aquila::tools::market_data {
namespace {

namespace cfg = aquila::config;
namespace md = aquila::market_data;
namespace chrono = std::chrono;

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

md::DataShmConfig MakeCombinedCreateConfig(std::string_view suffix) {
  return md::DataShmConfig{
      .enabled = true,
      .shm_name = UniqueShmName(suffix),
      .book_ticker_channel_name = "book_ticker_channel",
      .trade_channel_name = "trade_channel",
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

cfg::DataReaderSourceConfig MakeTradeSourceConfig(std::string name,
                                                  Exchange exchange,
                                                  std::string shm_name) {
  return cfg::DataReaderSourceConfig{
      .name = std::move(name),
      .type = cfg::DataReaderSourceType::kShm,
      .exchange = exchange,
      .feed = cfg::DataReaderFeed::kTrade,
      .shm_name = std::move(shm_name),
      .channel_name = "trade_channel",
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

Trade MakeTrade(std::int64_t id, Exchange exchange, std::int64_t exchange_ns,
                std::int64_t trade_ns, std::int64_t local_ns) noexcept {
  return Trade{
      .id = id,
      .symbol_id = 42,
      .exchange = exchange,
      .side = id % 2 == 0 ? OrderSide::kSell : OrderSide::kBuy,
      .reserved = 0,
      .exchange_ns = exchange_ns,
      .trade_ns = trade_ns,
      .local_ns = local_ns,
      .price = 65'000.0 + static_cast<double>(id),
      .volume = 0.1 + static_cast<double>(id),
      .batch_index = 0,
      .batch_count = 1,
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

void ExpectTradeEq(const Trade& actual, const Trade& expected) {
  EXPECT_EQ(actual.id, expected.id);
  EXPECT_EQ(actual.symbol_id, expected.symbol_id);
  EXPECT_EQ(actual.exchange, expected.exchange);
  EXPECT_EQ(actual.side, expected.side);
  EXPECT_EQ(actual.reserved, expected.reserved);
  EXPECT_EQ(actual.exchange_ns, expected.exchange_ns);
  EXPECT_EQ(actual.trade_ns, expected.trade_ns);
  EXPECT_EQ(actual.local_ns, expected.local_ns);
  EXPECT_DOUBLE_EQ(actual.price, expected.price);
  EXPECT_DOUBLE_EQ(actual.volume, expected.volume);
  EXPECT_EQ(actual.batch_index, expected.batch_index);
  EXPECT_EQ(actual.batch_count, expected.batch_count);
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

std::vector<Trade> ReadTrades(const std::filesystem::path& output_path) {
  const std::uintmax_t size = std::filesystem::file_size(output_path);
  EXPECT_EQ(size % sizeof(Trade), 0U);
  std::vector<Trade> records(size / sizeof(Trade));

  std::ifstream input(output_path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  if (!records.empty()) {
    input.read(reinterpret_cast<char*>(records.data()),
               static_cast<std::streamsize>(records.size() * sizeof(Trade)));
    EXPECT_TRUE(input.good());
  }
  return records;
}

std::vector<std::filesystem::path> FilesWithExtension(
    const std::filesystem::path& directory, std::string_view extension) {
  std::vector<std::filesystem::path> files;
  if (!std::filesystem::exists(directory)) {
    return files;
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() == extension) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    lines.push_back(line);
  }
  return lines;
}

TEST(DataReaderRecorderConfigTest, DefaultsToSingleFileRecorder) {
  const toml::parse_result parsed = toml::parse(R"toml(
[data_reader]
name = "unused"
)toml");
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() / "merged_book_ticker.bin";

  const auto result =
      ParseRecorderConfig(parsed, output_path, RecorderWriteMode::kTruncate);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_FALSE(result.value.rotation.enabled);
  EXPECT_EQ(result.value.rotation.rotation_interval_sec, 3600U);
  EXPECT_EQ(result.value.rotation.output_dir,
            output_path.parent_path() / "segments");
  EXPECT_EQ(result.value.rotation.file_prefix, "merged_book_ticker");
  EXPECT_EQ(result.value.rotation.manifest_path,
            output_path.parent_path() / "merged_book_ticker_manifest.jsonl");
  EXPECT_EQ(result.value.trade_rotation.output_dir,
            output_path.parent_path() / "segments_trade");
  EXPECT_EQ(result.value.trade_rotation.file_prefix, "merged_trade");
  EXPECT_EQ(result.value.trade_rotation.manifest_path,
            output_path.parent_path() / "merged_trade_manifest.jsonl");
}

TEST(DataReaderRecorderConfigTest, ParsesRotationDefaults) {
  const toml::parse_result parsed = toml::parse(R"toml(
[recorder]
rotation_enabled = true
)toml");
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() / "live.bin";

  const auto result =
      ParseRecorderConfig(parsed, output_path, RecorderWriteMode::kTruncate);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.rotation.enabled);
  EXPECT_EQ(result.value.rotation.rotation_interval_sec, 3600U);
  EXPECT_EQ(result.value.rotation.output_dir,
            output_path.parent_path() / "segments");
  EXPECT_EQ(result.value.rotation.file_prefix, "live");
  EXPECT_EQ(result.value.rotation.manifest_path,
            output_path.parent_path() / "live_manifest.jsonl");
  EXPECT_EQ(result.value.trade_rotation.output_dir,
            output_path.parent_path() / "segments_trade");
  EXPECT_EQ(result.value.trade_rotation.file_prefix, "live_trade");
  EXPECT_EQ(result.value.trade_rotation.manifest_path,
            output_path.parent_path() / "live_trade_manifest.jsonl");
}

TEST(DataReaderRecorderConfigTest, ParsesExplicitRotationFields) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "aquila_recorder_config_test";
  const std::filesystem::path output_dir = root / "out";
  const std::filesystem::path trade_output_dir = root / "trade_out";
  const std::filesystem::path manifest_path = root / "manifest.jsonl";
  const std::filesystem::path trade_manifest_path =
      root / "trade_manifest.jsonl";
  const std::string toml_text = fmt::format(
      R"toml(
[recorder]
rotation_enabled = true
rotation_interval_sec = 17
output_dir = "{}"
file_prefix = "book_ticker"
manifest_path = "{}"
trade_output_dir = "{}"
trade_file_prefix = "trade"
trade_manifest_path = "{}"
)toml",
      output_dir.string(), manifest_path.string(), trade_output_dir.string(),
      trade_manifest_path.string());
  const toml::parse_result parsed = toml::parse(toml_text);

  const auto result = ParseRecorderConfig(parsed, root / "ignored.bin",
                                          RecorderWriteMode::kTruncate);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.rotation.enabled);
  EXPECT_EQ(result.value.rotation.rotation_interval_sec, 17U);
  EXPECT_EQ(result.value.rotation.output_dir, output_dir);
  EXPECT_EQ(result.value.rotation.file_prefix, "book_ticker");
  EXPECT_EQ(result.value.rotation.manifest_path, manifest_path);
  EXPECT_EQ(result.value.trade_rotation.output_dir, trade_output_dir);
  EXPECT_EQ(result.value.trade_rotation.file_prefix, "trade");
  EXPECT_EQ(result.value.trade_rotation.manifest_path, trade_manifest_path);
}

TEST(DataReaderRecorderConfigTest, RejectsInvalidRotationInterval) {
  const toml::parse_result parsed = toml::parse(R"toml(
[recorder]
rotation_enabled = true
rotation_interval_sec = 0
)toml");

  const auto result = ParseRecorderConfig(parsed, "/home/liuxiang/tmp/live.bin",
                                          RecorderWriteMode::kTruncate);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("rotation_interval_sec"), std::string::npos);
}

TEST(DataReaderRecorderConfigTest, RejectsAppendModeWithRotation) {
  const toml::parse_result parsed = toml::parse(R"toml(
[recorder]
rotation_enabled = true
)toml");

  const auto result = ParseRecorderConfig(parsed, "/home/liuxiang/tmp/live.bin",
                                          RecorderWriteMode::kAppend);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("append"), std::string::npos);
}

TEST(DataReaderRecorderConfigTest, RejectsWrongTypedRecorderFields) {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"rotation_enabled", R"toml(
[recorder]
rotation_enabled = "true"
)toml"},
      {"rotation_interval_sec", R"toml(
[recorder]
rotation_interval_sec = "3600"
)toml"},
      {"output_dir", R"toml(
[recorder]
output_dir = 7
)toml"},
      {"file_prefix", R"toml(
[recorder]
file_prefix = false
)toml"},
      {"manifest_path", R"toml(
[recorder]
manifest_path = 9
)toml"},
      {"trade_output_dir", R"toml(
[recorder]
trade_output_dir = false
)toml"},
      {"trade_file_prefix", R"toml(
[recorder]
trade_file_prefix = 8
)toml"},
      {"trade_manifest_path", R"toml(
[recorder]
trade_manifest_path = true
)toml"},
  };

  for (const auto& [field_name, toml_text] : cases) {
    SCOPED_TRACE(field_name);
    const toml::parse_result parsed = toml::parse(toml_text);

    const auto result = ParseRecorderConfig(
        parsed, "/home/liuxiang/tmp/live.bin", RecorderWriteMode::kTruncate);

    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find(field_name), std::string::npos);
  }
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
     RealtimeReaderDrainsBookTickerAndTradeSourcesIntoSeparateBinaries) {
  const md::DataShmConfig combined_config =
      MakeCombinedCreateConfig("mixed_book_trade");
  ShmCleanup cleanup(combined_config.shm_name);
  md::DataShmPublisher publisher(combined_config);

  cfg::DataReaderConfig config;
  config.name = "mixed_recorder_integration_test";
  config.max_events_per_drain = 64;
  config.sources.push_back(MakeSourceConfig("gate_book_ticker", Exchange::kGate,
                                            combined_config.shm_name));
  config.sources.push_back(MakeTradeSourceConfig("gate_trade", Exchange::kGate,
                                                 combined_config.shm_name));

  using Reader = md::RealtimeDataReader<md::RealtimeDataReaderDiagnostics>;
  Reader reader(std::move(config));

  const BookTicker first_book =
      MakeTicker(101, Exchange::kGate, 1'770'000'000'000'000'101,
                 1'770'000'000'000'010'101);
  const BookTicker second_book =
      MakeTicker(102, Exchange::kGate, 1'770'000'000'000'000'102,
                 1'770'000'000'000'010'102);
  const Trade first_trade =
      MakeTrade(201, Exchange::kGate, 1'770'000'000'000'000'201,
                1'770'000'000'000'005'201, 1'770'000'000'000'010'201);
  const Trade second_trade =
      MakeTrade(202, Exchange::kGate, 1'770'000'000'000'000'202,
                1'770'000'000'000'005'202, 1'770'000'000'000'010'202);

  publisher.OnBookTicker(first_book);
  publisher.OnBookTicker(second_book);
  publisher.OnTrade(first_trade);
  publisher.OnTrade(second_trade);

  const std::filesystem::path book_output =
      std::filesystem::temp_directory_path() /
      ("aquila_data_reader_recorder_mixed_book_" + std::to_string(::getpid()) +
       ".bin");
  const std::filesystem::path trade_output =
      std::filesystem::temp_directory_path() /
      ("aquila_data_reader_recorder_mixed_trade_" + std::to_string(::getpid()) +
       ".bin");
  std::filesystem::remove(book_output);
  std::filesystem::remove(trade_output);

  {
    BinaryRecorder recorder(book_output, trade_output,
                            RecorderWriteMode::kTruncate);
    EXPECT_EQ(reader.Drain(recorder, config.max_events_per_drain), 4U);
    EXPECT_TRUE(recorder.Flush());
    EXPECT_FALSE(recorder.write_error());

    EXPECT_EQ(recorder.book_ticker_stats().total_records, 2U);
    EXPECT_EQ(recorder.trade_stats().total_records, 2U);
    EXPECT_EQ(recorder.book_ticker_stats().RecordsForExchange(Exchange::kGate),
              2U);
    EXPECT_EQ(recorder.trade_stats().RecordsForExchange(Exchange::kGate), 2U);

    const md::RealtimeDataReaderStats& reader_stats =
        reader.diagnostics().stats();
    EXPECT_EQ(reader_stats.total_count, 4U);
    ASSERT_EQ(reader_stats.sources.size(), 2U);
    EXPECT_EQ(reader_stats.sources[0].book_ticker_count, 2U);
    EXPECT_EQ(reader_stats.sources[1].trade_count, 2U);
  }

  const std::vector<BookTicker> book_records = ReadBookTickers(book_output);
  ASSERT_EQ(book_records.size(), 2U);
  ExpectBookTickerEq(book_records[0], first_book);
  ExpectBookTickerEq(book_records[1], second_book);

  const std::vector<Trade> trade_records = ReadTrades(trade_output);
  ASSERT_EQ(trade_records.size(), 2U);
  ExpectTradeEq(trade_records[0], first_trade);
  ExpectTradeEq(trade_records[1], second_trade);

  std::filesystem::remove(book_output);
  std::filesystem::remove(trade_output);
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

TEST(DataReaderRecorderTest, DerivesTradeOutputPathFromBookTickerOutputPath) {
  EXPECT_EQ(DeriveTradeOutputPath("/home/liuxiang/tmp/live_book_ticker.bin"),
            std::filesystem::path{"/home/liuxiang/tmp/live_trade.bin"});
  EXPECT_EQ(DeriveTradeOutputPath("/home/liuxiang/tmp/live.bin"),
            std::filesystem::path{"/home/liuxiang/tmp/live_trade.bin"});
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

TEST(DataReaderRecorderTest, RotatingRecorderKeepsActiveTmpOutOfManifest) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("aquila_rotating_recorder_active_test_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);

  RecorderRotationConfig rotation{
      .enabled = true,
      .rotation_interval_sec = 60,
      .output_dir = root / "segments",
      .file_prefix = "book_ticker",
      .manifest_path = root / "manifest.jsonl",
  };
  RecorderTimeSnapshot now{
      .steady = chrono::steady_clock::time_point{chrono::seconds{0}},
      .wall = chrono::sys_seconds{chrono::seconds{1'779'669'600}},
  };
  RotatingBookTickerBinaryRecorder recorder(rotation, [&] { return now; });

  recorder.OnBookTicker(MakeTicker(1, Exchange::kGate, 10, 20));

  EXPECT_EQ(FilesWithExtension(rotation.output_dir, ".tmp").size(), 1U);
  EXPECT_TRUE(FilesWithExtension(rotation.output_dir, ".bin").empty());
  EXPECT_FALSE(std::filesystem::exists(rotation.manifest_path));
  EXPECT_TRUE(recorder.Flush());

  std::filesystem::remove_all(root);
}

TEST(DataReaderRecorderTest, RotatingRecorderRejectsExistingActiveTmpSegment) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("aquila_rotating_recorder_collision_test_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);

  RecorderRotationConfig rotation{
      .enabled = true,
      .rotation_interval_sec = 60,
      .output_dir = root / "segments",
      .file_prefix = "book_ticker",
      .manifest_path = root / "manifest.jsonl",
  };
  const std::filesystem::path expected_tmp =
      rotation.output_dir / "book_ticker_19700101_000000_000001.bin.tmp";
  std::filesystem::create_directories(rotation.output_dir);
  {
    std::ofstream existing(expected_tmp, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(existing.is_open());
    existing << "existing";
  }

  RecorderTimeSnapshot now{
      .steady = chrono::steady_clock::time_point{chrono::seconds{0}},
      .wall = chrono::sys_seconds{chrono::seconds{0}},
  };
  EXPECT_THROW(
      RotatingBookTickerBinaryRecorder recorder(rotation, [&] { return now; }),
      std::runtime_error);
  EXPECT_EQ(std::filesystem::file_size(expected_tmp), 8U);

  std::filesystem::remove_all(root);
}

TEST(DataReaderRecorderTest, RotatingRecorderFinalizesSegmentsAndManifest) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("aquila_rotating_recorder_manifest_test_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);

  RecorderRotationConfig rotation{
      .enabled = true,
      .rotation_interval_sec = 1,
      .output_dir = root / "segments",
      .file_prefix = "book_ticker",
      .manifest_path = root / "manifest.jsonl",
  };
  RecorderTimeSnapshot now{
      .steady = chrono::steady_clock::time_point{chrono::seconds{0}},
      .wall = chrono::sys_seconds{chrono::seconds{1'779'669'600}},
  };
  RotatingBookTickerBinaryRecorder recorder(rotation, [&] { return now; });

  const BookTicker first = MakeTicker(1, Exchange::kGate, 100, 200);
  const BookTicker second = MakeTicker(2, Exchange::kBinance, 300, 400);
  recorder.OnBookTicker(first);
  now.steady += chrono::seconds{2};
  now.wall += chrono::seconds{2};
  recorder.OnBookTicker(second);
  EXPECT_TRUE(recorder.Flush());
  EXPECT_FALSE(recorder.write_error());

  const std::vector<std::filesystem::path> tmp_files =
      FilesWithExtension(rotation.output_dir, ".tmp");
  const std::vector<std::filesystem::path> bin_files =
      FilesWithExtension(rotation.output_dir, ".bin");
  ASSERT_TRUE(tmp_files.empty());
  ASSERT_EQ(bin_files.size(), 2U);
  ASSERT_EQ(ReadBookTickers(bin_files[0]).size(), 1U);
  ASSERT_EQ(ReadBookTickers(bin_files[1]).size(), 1U);
  ExpectBookTickerEq(ReadBookTickers(bin_files[0])[0], first);
  ExpectBookTickerEq(ReadBookTickers(bin_files[1])[0], second);

  const std::vector<std::string> lines = ReadLines(rotation.manifest_path);
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_NE(lines[0].find(R"("sequence":1)"), std::string::npos);
  EXPECT_NE(lines[0].find(R"("records":1)"), std::string::npos);
  EXPECT_NE(lines[0].find(R"("closed_reason":"rotation_interval")"),
            std::string::npos);
  EXPECT_NE(lines[1].find(R"("sequence":2)"), std::string::npos);
  EXPECT_NE(lines[1].find(R"("records":1)"), std::string::npos);
  EXPECT_NE(lines[1].find(R"("closed_reason":"flush")"), std::string::npos);

  EXPECT_EQ(recorder.stats().total_records, 2U);
  EXPECT_EQ(recorder.segments_completed(), 2U);

  std::filesystem::remove_all(root);
}

TEST(DataReaderRecorderTest,
     RotatingBinaryRecorderFinalizesBookTickerAndTradeSegments) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("aquila_rotating_binary_recorder_mixed_test_" +
       std::to_string(::getpid()));
  std::filesystem::remove_all(root);

  RecorderRotationConfig book_rotation{
      .enabled = true,
      .rotation_interval_sec = 1,
      .output_dir = root / "book_segments",
      .file_prefix = "book_ticker",
      .manifest_path = root / "book_manifest.jsonl",
  };
  RecorderRotationConfig trade_rotation{
      .enabled = true,
      .rotation_interval_sec = 1,
      .output_dir = root / "trade_segments",
      .file_prefix = "trade",
      .manifest_path = root / "trade_manifest.jsonl",
  };
  RecorderTimeSnapshot now{
      .steady = chrono::steady_clock::time_point{chrono::seconds{0}},
      .wall = chrono::sys_seconds{chrono::seconds{1'779'669'600}},
  };
  RotatingBinaryRecorder recorder(book_rotation, trade_rotation,
                                  [&] { return now; });

  const BookTicker first_book = MakeTicker(1, Exchange::kGate, 100, 200);
  const BookTicker second_book = MakeTicker(2, Exchange::kBinance, 300, 400);
  const Trade first_trade = MakeTrade(11, Exchange::kGate, 110, 115, 120);
  const Trade second_trade = MakeTrade(12, Exchange::kBinance, 310, 315, 320);

  recorder.OnBookTicker(first_book);
  recorder.OnTrade(first_trade);
  now.steady += chrono::seconds{2};
  now.wall += chrono::seconds{2};
  recorder.OnBookTicker(second_book);
  recorder.OnTrade(second_trade);

  EXPECT_TRUE(recorder.Flush());
  EXPECT_FALSE(recorder.write_error());

  const std::vector<std::filesystem::path> book_tmp_files =
      FilesWithExtension(book_rotation.output_dir, ".tmp");
  const std::vector<std::filesystem::path> trade_tmp_files =
      FilesWithExtension(trade_rotation.output_dir, ".tmp");
  const std::vector<std::filesystem::path> book_bin_files =
      FilesWithExtension(book_rotation.output_dir, ".bin");
  const std::vector<std::filesystem::path> trade_bin_files =
      FilesWithExtension(trade_rotation.output_dir, ".bin");
  ASSERT_TRUE(book_tmp_files.empty());
  ASSERT_TRUE(trade_tmp_files.empty());
  ASSERT_EQ(book_bin_files.size(), 2U);
  ASSERT_EQ(trade_bin_files.size(), 2U);
  ExpectBookTickerEq(ReadBookTickers(book_bin_files[0])[0], first_book);
  ExpectBookTickerEq(ReadBookTickers(book_bin_files[1])[0], second_book);
  ExpectTradeEq(ReadTrades(trade_bin_files[0])[0], first_trade);
  ExpectTradeEq(ReadTrades(trade_bin_files[1])[0], second_trade);

  const std::vector<std::string> book_lines =
      ReadLines(book_rotation.manifest_path);
  const std::vector<std::string> trade_lines =
      ReadLines(trade_rotation.manifest_path);
  ASSERT_EQ(book_lines.size(), 2U);
  ASSERT_EQ(trade_lines.size(), 2U);
  EXPECT_NE(book_lines[0].find(R"("sequence":1)"), std::string::npos);
  EXPECT_NE(trade_lines[0].find(R"("sequence":1)"), std::string::npos);
  EXPECT_NE(book_lines[1].find(R"("closed_reason":"flush")"),
            std::string::npos);
  EXPECT_NE(trade_lines[1].find(R"("closed_reason":"flush")"),
            std::string::npos);

  EXPECT_EQ(recorder.book_ticker_stats().total_records, 2U);
  EXPECT_EQ(recorder.trade_stats().total_records, 2U);
  EXPECT_EQ(recorder.book_ticker_segments_completed(), 2U);
  EXPECT_EQ(recorder.trade_segments_completed(), 2U);

  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace aquila::tools::market_data
