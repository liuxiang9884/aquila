#include "core/market_data/binary_data_reader.h"

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace {

namespace cfg = aquila::config;
namespace md = aquila::market_data;

class TempDir {
 public:
  TempDir()
      : path_(std::filesystem::temp_directory_path() /
              fmt::format("aquila_binary_data_reader_test_{}_{}", ::getpid(),
                          next_id_++)) {
    std::filesystem::create_directories(path_);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path File(std::string_view name) const {
    return path_ / std::string{name};
  }

 private:
  std::filesystem::path path_;
  inline static std::uint32_t next_id_{0};
};

struct RecordingHandler {
  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    book_tickers.push_back(book_ticker);
  }

  std::vector<aquila::BookTicker> book_tickers;
};

aquila::BookTicker MakeBookTicker(std::int64_t id, aquila::Exchange exchange) {
  return aquila::BookTicker{
      .id = id,
      .symbol_id = static_cast<std::int32_t>(100 + id),
      .exchange = exchange,
      .exchange_ns = 1'770'000'000'000'000'000 + id,
      .local_ns = 1'770'000'000'000'100'000 + id,
      .bid_price = 65'000.25 + static_cast<double>(id),
      .bid_volume = 10.5 + static_cast<double>(id),
      .ask_price = 65'001.75 + static_cast<double>(id),
      .ask_volume = 11.5 + static_cast<double>(id),
  };
}

void WriteBookTickerFile(const std::filesystem::path& path,
                         const std::vector<aquila::BookTicker>& records) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  if (!records.empty()) {
    output.write(reinterpret_cast<const char*>(records.data()),
                 static_cast<std::streamsize>(records.size() *
                                              sizeof(aquila::BookTicker)));
  }
  ASSERT_TRUE(output.good()) << path;
}

void WriteTrailingByteFile(const std::filesystem::path& path) {
  const aquila::BookTicker record = MakeBookTicker(1, aquila::Exchange::kGate);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  output.write(reinterpret_cast<const char*>(&record), sizeof(record));
  const char trailing = '\x01';
  output.write(&trailing, sizeof(trailing));
  ASSERT_TRUE(output.good()) << path;
}

cfg::DataReaderConfig MakeBinaryReaderConfig(
    std::vector<std::filesystem::path> files,
    std::uint32_t max_events_per_source = 64) {
  cfg::DataReaderConfig config;
  config.name = "binary_reader";
  config.max_events_per_source = max_events_per_source;
  config.sources.push_back(cfg::DataReaderSourceConfig{
      .name = "binary_book_ticker",
      .type = cfg::DataReaderSourceType::kBinaryFile,
      .exchange = aquila::Exchange::kGate,
      .feed = cfg::DataReaderFeed::kBookTicker,
      .shm_name = {},
      .channel_name = {},
      .files = std::move(files),
      .start_position = cfg::DataReaderStartPosition::kEarliestVisible,
      .read_mode = cfg::DataReaderReadMode::kDrain,
      .required = true,
  });
  return config;
}

void ExpectBookTickerEquals(const aquila::BookTicker& actual,
                            const aquila::BookTicker& expected) {
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

TEST(BinaryDataReaderTest, ReadsMultipleFilesSequentially) {
  TempDir temp_dir;
  const std::filesystem::path first_file = temp_dir.File("first.bin");
  const std::filesystem::path second_file = temp_dir.File("second.bin");
  const std::vector<aquila::BookTicker> expected{
      MakeBookTicker(1, aquila::Exchange::kGate),
      MakeBookTicker(2, aquila::Exchange::kBinance),
      MakeBookTicker(3, aquila::Exchange::kGate),
  };
  WriteBookTickerFile(first_file, {expected[0], expected[1]});
  WriteBookTickerFile(second_file, {expected[2]});

  md::BinaryDataReader<md::BinaryDataReaderDiagnostics> reader(
      MakeBinaryReaderConfig({first_file, second_file}));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 3U);
  ASSERT_EQ(handler.book_tickers.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    ExpectBookTickerEquals(handler.book_tickers[i], expected[i]);
  }

  const md::BinaryDataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.poll_calls, 1U);
  EXPECT_EQ(stats.empty_polls, 0U);
  EXPECT_EQ(stats.book_tickers, 3U);
  EXPECT_EQ(stats.files_completed, 2U);
}

TEST(BinaryDataReaderTest, RespectsMaxEventsPerSource) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("records.bin");
  WriteBookTickerFile(file, {MakeBookTicker(10, aquila::Exchange::kGate),
                             MakeBookTicker(11, aquila::Exchange::kGate),
                             MakeBookTicker(12, aquila::Exchange::kGate)});

  md::BinaryDataReader reader(MakeBinaryReaderConfig({file}, 2));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 2U);
  ASSERT_EQ(handler.book_tickers.size(), 2U);
  EXPECT_EQ(handler.book_tickers[0].id, 10);
  EXPECT_EQ(handler.book_tickers[1].id, 11);

  handler.book_tickers.clear();
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  EXPECT_EQ(handler.book_tickers[0].id, 12);
}

TEST(BinaryDataReaderTest, EmptyPollAfterAllFilesCompleted) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("single.bin");
  WriteBookTickerFile(file, {MakeBookTicker(20, aquila::Exchange::kGate)});

  md::BinaryDataReader<md::BinaryDataReaderDiagnostics> reader(
      MakeBinaryReaderConfig({file}));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  handler.book_tickers.clear();
  EXPECT_EQ(reader.Poll(handler), 0U);
  EXPECT_TRUE(handler.book_tickers.empty());

  const md::BinaryDataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.poll_calls, 2U);
  EXPECT_EQ(stats.empty_polls, 1U);
  EXPECT_EQ(stats.book_tickers, 1U);
  EXPECT_EQ(stats.files_completed, 1U);
}

TEST(BinaryDataReaderTest, DoesNotModifyRecordFields) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("exact.bin");
  const aquila::BookTicker expected = aquila::BookTicker{
      .id = 987'654'321,
      .symbol_id = 42,
      .exchange = aquila::Exchange::kBinance,
      .exchange_ns = 1'770'000'000'123'456'789,
      .local_ns = 1'770'000'000'223'456'789,
      .bid_price = 12345.125,
      .bid_volume = 0.875,
      .ask_price = 12345.625,
      .ask_volume = 1.125,
  };
  WriteBookTickerFile(file, {expected});

  md::BinaryDataReader reader(MakeBinaryReaderConfig({file}));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  ExpectBookTickerEquals(handler.book_tickers[0], expected);
}

TEST(BinaryDataReaderTest, RejectsFileWithTrailingBytes) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("trailing.bin");
  WriteTrailingByteFile(file);

  EXPECT_THROW((md::BinaryDataReader{MakeBinaryReaderConfig({file})}),
               std::runtime_error);
}

}  // namespace
