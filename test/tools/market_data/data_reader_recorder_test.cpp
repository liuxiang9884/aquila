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
}

TEST(DataReaderRecorderConfigTest, ParsesExplicitRotationFields) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "aquila_recorder_config_test";
  const std::filesystem::path output_dir = root / "out";
  const std::filesystem::path manifest_path = root / "manifest.jsonl";
  const std::string toml_text = fmt::format(
      R"toml(
[recorder]
rotation_enabled = true
rotation_interval_sec = 17
output_dir = "{}"
file_prefix = "book_ticker"
manifest_path = "{}"
)toml",
      output_dir.string(), manifest_path.string());
  const toml::parse_result parsed = toml::parse(toml_text);

  const auto result = ParseRecorderConfig(parsed, root / "ignored.bin",
                                          RecorderWriteMode::kTruncate);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.rotation.enabled);
  EXPECT_EQ(result.value.rotation.rotation_interval_sec, 17U);
  EXPECT_EQ(result.value.rotation.output_dir, output_dir);
  EXPECT_EQ(result.value.rotation.file_prefix, "book_ticker");
  EXPECT_EQ(result.value.rotation.manifest_path, manifest_path);
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

}  // namespace
}  // namespace aquila::tools::market_data
