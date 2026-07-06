#include "core/market_data/historical_data_reader.h"

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

#include "core/market_data/data_reader_concepts.h"
#include "core/market_data/market_data_binary_format.h"

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

  void OnTrade(const aquila::Trade& trade) noexcept {
    trades.push_back(trade);
  }

  std::vector<aquila::BookTicker> book_tickers;
  std::vector<aquila::Trade> trades;
};

static_assert(md::DataReaderLike<md::HistoricalDataReader<>, RecordingHandler>);
static_assert(md::FiniteDataReader<md::HistoricalDataReader<>>);
static_assert(noexcept(std::declval<md::HistoricalDataReader<>&>().Poll(
    std::declval<RecordingHandler&>())));
static_assert(noexcept(std::declval<md::HistoricalDataReader<>&>().Drain(
    std::declval<RecordingHandler&>(), std::uint64_t{1})));

template <typename StatsT>
concept HasPollDiagnostics = requires(StatsT stats) {
  stats.poll_calls;
  stats.empty_polls;
};

template <typename StatsT>
concept HasOldBookTickersField = requires(StatsT stats) { stats.book_tickers; };

static_assert(!HasPollDiagnostics<md::HistoricalDataReaderStats>);
static_assert(!HasOldBookTickersField<md::HistoricalDataReaderStats>);

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

aquila::Trade MakeTrade(std::int64_t id, aquila::Exchange exchange) {
  return aquila::Trade{
      .id = id,
      .symbol_id = static_cast<std::int32_t>(200 + id),
      .exchange = exchange,
      .side = id % 2 == 0 ? aquila::OrderSide::kBuy
                          : aquila::OrderSide::kSell,
      .reserved = 0,
      .exchange_ns = 1'770'000'000'001'000'000 + id,
      .trade_ns = 1'770'000'000'001'100'000 + id,
      .local_ns = 1'770'000'000'001'200'000 + id,
      .price = 66'000.25 + static_cast<double>(id),
      .volume = 0.001 + static_cast<double>(id) * 0.0001,
      .batch_index = static_cast<std::uint32_t>(id % 4),
      .batch_count = 4,
  };
}

TEST(MarketDataBinaryFormatTest, BuildsBookTickerHeader) {
  const md::MarketDataBinaryHeader header =
      md::MakeMarketDataBinaryHeader(cfg::DataReaderFeed::kBookTicker);

  EXPECT_EQ(header.magic, md::kMarketDataBinaryMagic);
  EXPECT_EQ(header.version, md::kMarketDataBinaryVersion);
  EXPECT_EQ(header.header_size, sizeof(md::MarketDataBinaryHeader));
  EXPECT_EQ(header.feed_type, md::kMarketDataBinaryBookTickerFeedType);
  EXPECT_EQ(header.record_size, sizeof(aquila::BookTicker));
  EXPECT_EQ(header.flags, 0U);
}

TEST(MarketDataBinaryFormatTest, BuildsTradeHeader) {
  const md::MarketDataBinaryHeader header =
      md::MakeMarketDataBinaryHeader(cfg::DataReaderFeed::kTrade);

  EXPECT_EQ(header.magic, md::kMarketDataBinaryMagic);
  EXPECT_EQ(header.version, md::kMarketDataBinaryVersion);
  EXPECT_EQ(header.header_size, sizeof(md::MarketDataBinaryHeader));
  EXPECT_EQ(header.feed_type, md::kMarketDataBinaryTradeFeedType);
  EXPECT_EQ(header.record_size, sizeof(aquila::Trade));
  EXPECT_EQ(header.flags, 0U);
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

void WriteTradeFile(const std::filesystem::path& path,
                    const std::vector<aquila::Trade>& records) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  if (!records.empty()) {
    output.write(reinterpret_cast<const char*>(records.data()),
                 static_cast<std::streamsize>(records.size() *
                                              sizeof(aquila::Trade)));
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

void WriteTradeTrailingByteFile(const std::filesystem::path& path) {
  const aquila::Trade record = MakeTrade(1, aquila::Exchange::kGate);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  output.write(reinterpret_cast<const char*>(&record), sizeof(record));
  const char trailing = '\x01';
  output.write(&trailing, sizeof(trailing));
  ASSERT_TRUE(output.good()) << path;
}

cfg::DataReaderConfig MakeBinaryReaderConfig(
    std::vector<std::filesystem::path> files,
    std::uint32_t max_events_per_drain = 64) {
  cfg::DataReaderConfig config;
  config.name = "binary_reader";
  config.max_events_per_drain = max_events_per_drain;
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

cfg::DataReaderConfig MakeTradeBinaryReaderConfig(
    std::vector<std::filesystem::path> files,
    std::uint32_t max_events_per_drain = 64) {
  cfg::DataReaderConfig config;
  config.name = "binary_trade_reader";
  config.max_events_per_drain = max_events_per_drain;
  config.sources.push_back(cfg::DataReaderSourceConfig{
      .name = "binary_trade",
      .type = cfg::DataReaderSourceType::kBinaryFile,
      .exchange = aquila::Exchange::kGate,
      .feed = cfg::DataReaderFeed::kTrade,
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

void ExpectTradeEquals(const aquila::Trade& actual,
                       const aquila::Trade& expected) {
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

TEST(HistoricalDataReaderTest, PollReadsOneRecordAtATimeAcrossFiles) {
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

  md::HistoricalDataReader<md::HistoricalDataReaderDiagnostics> reader(
      MakeBinaryReaderConfig({first_file, second_file}));
  EXPECT_FALSE(reader.finished());

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  ExpectBookTickerEquals(handler.book_tickers[0], expected[0]);
  EXPECT_FALSE(reader.finished());

  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 2U);
  ExpectBookTickerEquals(handler.book_tickers[1], expected[1]);
  EXPECT_FALSE(reader.finished());

  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), expected.size());
  ExpectBookTickerEquals(handler.book_tickers[2], expected[2]);
  EXPECT_TRUE(reader.finished());

  EXPECT_EQ(reader.Poll(handler), 0U);
  EXPECT_EQ(handler.book_tickers.size(), expected.size());
  EXPECT_TRUE(reader.finished());

  const md::HistoricalDataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.total_count, 3U);
  EXPECT_EQ(stats.files_completed, 2U);
}

TEST(HistoricalDataReaderTest, DrainReadsAtMostMaxEvents) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("records.bin");
  WriteBookTickerFile(file, {MakeBookTicker(10, aquila::Exchange::kGate),
                             MakeBookTicker(11, aquila::Exchange::kGate),
                             MakeBookTicker(12, aquila::Exchange::kGate)});

  md::HistoricalDataReader reader(MakeBinaryReaderConfig({file}, 2));

  RecordingHandler handler;
  EXPECT_EQ(reader.Drain(handler, 0), 0U);
  EXPECT_TRUE(handler.book_tickers.empty());

  EXPECT_EQ(reader.Drain(handler, 2), 2U);
  ASSERT_EQ(handler.book_tickers.size(), 2U);
  EXPECT_EQ(handler.book_tickers[0].id, 10);
  EXPECT_EQ(handler.book_tickers[1].id, 11);
  EXPECT_FALSE(reader.finished());

  handler.book_tickers.clear();
  EXPECT_EQ(reader.Drain(handler, 10), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  EXPECT_EQ(handler.book_tickers[0].id, 12);
  EXPECT_TRUE(reader.finished());
}

TEST(HistoricalDataReaderTest, DrainContinuesAcrossFileBoundaries) {
  TempDir temp_dir;
  const std::filesystem::path first_file = temp_dir.File("first.bin");
  const std::filesystem::path second_file = temp_dir.File("second.bin");
  const std::filesystem::path empty_file = temp_dir.File("empty.bin");
  const std::vector<aquila::BookTicker> expected{
      MakeBookTicker(30, aquila::Exchange::kGate),
      MakeBookTicker(31, aquila::Exchange::kGate),
      MakeBookTicker(32, aquila::Exchange::kBinance),
  };
  WriteBookTickerFile(first_file, {expected[0]});
  WriteBookTickerFile(empty_file, {});
  WriteBookTickerFile(second_file, {expected[1], expected[2]});

  md::HistoricalDataReader<md::HistoricalDataReaderDiagnostics> reader(
      MakeBinaryReaderConfig({first_file, empty_file, second_file}, 8));

  RecordingHandler handler;
  EXPECT_EQ(reader.Drain(handler, 8), expected.size());
  ASSERT_EQ(handler.book_tickers.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    ExpectBookTickerEquals(handler.book_tickers[i], expected[i]);
  }
  EXPECT_TRUE(reader.finished());

  const md::HistoricalDataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.total_count, expected.size());
  EXPECT_EQ(stats.files_completed, 3U);
}

TEST(HistoricalDataReaderTest, PollReadsTradeRecordsAcrossFiles) {
  TempDir temp_dir;
  const std::filesystem::path first_file = temp_dir.File("first_trade.bin");
  const std::filesystem::path second_file = temp_dir.File("second_trade.bin");
  const std::vector<aquila::Trade> expected{
      MakeTrade(1, aquila::Exchange::kGate),
      MakeTrade(2, aquila::Exchange::kBinance),
      MakeTrade(3, aquila::Exchange::kGate),
  };
  WriteTradeFile(first_file, {expected[0], expected[1]});
  WriteTradeFile(second_file, {expected[2]});

  md::HistoricalDataReader<md::HistoricalDataReaderDiagnostics> reader(
      MakeTradeBinaryReaderConfig({first_file, second_file}));
  EXPECT_FALSE(reader.finished());

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.trades.size(), 1U);
  ExpectTradeEquals(handler.trades[0], expected[0]);
  EXPECT_TRUE(handler.book_tickers.empty());
  EXPECT_FALSE(reader.finished());

  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.trades.size(), 2U);
  ExpectTradeEquals(handler.trades[1], expected[1]);
  EXPECT_FALSE(reader.finished());

  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.trades.size(), expected.size());
  ExpectTradeEquals(handler.trades[2], expected[2]);
  EXPECT_TRUE(reader.finished());

  EXPECT_EQ(reader.Poll(handler), 0U);
  EXPECT_EQ(handler.trades.size(), expected.size());
  EXPECT_TRUE(reader.finished());

  const md::HistoricalDataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.total_count, 3U);
  EXPECT_EQ(stats.book_ticker_count, 0U);
  EXPECT_EQ(stats.trade_count, 3U);
  EXPECT_EQ(stats.files_completed, 2U);
}

TEST(HistoricalDataReaderTest, DrainReadsTradeRecordsAtMostMaxEvents) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("trades.bin");
  WriteTradeFile(file, {MakeTrade(10, aquila::Exchange::kGate),
                        MakeTrade(11, aquila::Exchange::kGate),
                        MakeTrade(12, aquila::Exchange::kGate)});

  md::HistoricalDataReader<md::HistoricalDataReaderDiagnostics> reader(
      MakeTradeBinaryReaderConfig({file}, 2));

  RecordingHandler handler;
  EXPECT_EQ(reader.Drain(handler, 0), 0U);
  EXPECT_TRUE(handler.trades.empty());

  EXPECT_EQ(reader.Drain(handler, 2), 2U);
  ASSERT_EQ(handler.trades.size(), 2U);
  EXPECT_EQ(handler.trades[0].id, 10);
  EXPECT_EQ(handler.trades[1].id, 11);
  EXPECT_FALSE(reader.finished());

  handler.trades.clear();
  EXPECT_EQ(reader.Drain(handler, 10), 1U);
  ASSERT_EQ(handler.trades.size(), 1U);
  EXPECT_EQ(handler.trades[0].id, 12);
  EXPECT_TRUE(reader.finished());

  const md::HistoricalDataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.total_count, 3U);
  EXPECT_EQ(stats.book_ticker_count, 0U);
  EXPECT_EQ(stats.trade_count, 3U);
  EXPECT_EQ(stats.files_completed, 1U);
}

TEST(HistoricalDataReaderTest, EmptyPollAfterAllFilesCompleted) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("single.bin");
  WriteBookTickerFile(file, {MakeBookTicker(20, aquila::Exchange::kGate)});

  md::HistoricalDataReader<md::HistoricalDataReaderDiagnostics> reader(
      MakeBinaryReaderConfig({file}));
  EXPECT_FALSE(reader.finished());

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  EXPECT_TRUE(reader.finished());
  handler.book_tickers.clear();
  EXPECT_EQ(reader.Poll(handler), 0U);
  EXPECT_TRUE(handler.book_tickers.empty());
  EXPECT_TRUE(reader.finished());

  const md::HistoricalDataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.total_count, 1U);
  EXPECT_EQ(stats.files_completed, 1U);
}

TEST(HistoricalDataReaderTest, EmptyFileIsCompletedAfterConstruction) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("empty.bin");
  WriteBookTickerFile(file, {});

  md::HistoricalDataReader<md::HistoricalDataReaderDiagnostics> reader(
      MakeBinaryReaderConfig({file}));

  EXPECT_TRUE(reader.finished());
  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 0U);
  EXPECT_TRUE(handler.book_tickers.empty());

  const md::HistoricalDataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.total_count, 0U);
  EXPECT_EQ(stats.files_completed, 1U);
}

TEST(HistoricalDataReaderTest, TrailingEmptyFilesCompleteWithLastDataRecord) {
  TempDir temp_dir;
  const std::filesystem::path data_file = temp_dir.File("data.bin");
  const std::filesystem::path empty_first = temp_dir.File("empty_first.bin");
  const std::filesystem::path empty_second = temp_dir.File("empty_second.bin");
  const aquila::BookTicker expected =
      MakeBookTicker(21, aquila::Exchange::kGate);
  WriteBookTickerFile(data_file, {expected});
  WriteBookTickerFile(empty_first, {});
  WriteBookTickerFile(empty_second, {});

  md::HistoricalDataReader<md::HistoricalDataReaderDiagnostics> reader(
      MakeBinaryReaderConfig({data_file, empty_first, empty_second}));
  EXPECT_FALSE(reader.finished());

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  ExpectBookTickerEquals(handler.book_tickers[0], expected);
  EXPECT_TRUE(reader.finished());

  const md::HistoricalDataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.total_count, 1U);
  EXPECT_EQ(stats.files_completed, 3U);
}

TEST(HistoricalDataReaderTest, DoesNotModifyRecordFields) {
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

  md::HistoricalDataReader reader(MakeBinaryReaderConfig({file}));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  ExpectBookTickerEquals(handler.book_tickers[0], expected);
}

TEST(HistoricalDataReaderTest, RejectsFileWithTrailingBytes) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("trailing.bin");
  WriteTrailingByteFile(file);

  EXPECT_THROW((md::HistoricalDataReader{MakeBinaryReaderConfig({file})}),
               std::runtime_error);
}

TEST(HistoricalDataReaderTest, RejectsTradeFileWithTrailingBytes) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("trailing_trade.bin");
  WriteTradeTrailingByteFile(file);

  EXPECT_THROW((md::HistoricalDataReader{MakeTradeBinaryReaderConfig({file})}),
               std::runtime_error);
}

TEST(HistoricalDataReaderTest, RejectsEmptySources) {
  cfg::DataReaderConfig config;
  config.name = "binary_reader";

  EXPECT_THROW((md::HistoricalDataReader{std::move(config)}),
               std::invalid_argument);
}

TEST(HistoricalDataReaderTest, RejectsMultipleSources) {
  TempDir temp_dir;
  const std::filesystem::path first_file = temp_dir.File("first.bin");
  const std::filesystem::path second_file = temp_dir.File("second.bin");
  WriteBookTickerFile(first_file, {MakeBookTicker(1, aquila::Exchange::kGate)});
  WriteBookTickerFile(second_file,
                      {MakeBookTicker(2, aquila::Exchange::kBinance)});

  cfg::DataReaderConfig config = MakeBinaryReaderConfig({first_file});
  config.sources.push_back(config.sources.front());
  config.sources.back().name = "second_binary_book_ticker";
  config.sources.back().files = {second_file};

  EXPECT_THROW((md::HistoricalDataReader{std::move(config)}),
               std::invalid_argument);
}

}  // namespace
