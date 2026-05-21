#include "core/market_data/realtime_data_reader.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/market_data/data_reader_concepts.h"
#include "core/market_data/data_shm.h"

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

struct RecordingHandler {
  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    book_tickers.push_back(book_ticker);
  }

  std::vector<aquila::BookTicker> book_tickers;
};

struct FinishedOnlyReader {
  [[nodiscard]] bool finished() const noexcept {
    return false;
  }
};

static_assert(md::DataReaderLike<md::RealtimeDataReader<>, RecordingHandler>);
static_assert(!md::FiniteDataReader<md::RealtimeDataReader<>>);
static_assert(!md::FiniteDataReader<FinishedOnlyReader>);
static_assert(noexcept(std::declval<md::RealtimeDataReader<>&>().Poll(
    std::declval<RecordingHandler&>())));
static_assert(noexcept(std::declval<md::RealtimeDataReader<>&>().Drain(
    std::declval<RecordingHandler&>(), std::uint64_t{1})));

template <typename StatsT>
concept HasPollDiagnostics = requires(StatsT stats) {
  stats.poll_calls;
  stats.empty_polls;
};

template <typename StatsT>
concept HasOldBookTickersField = requires(StatsT stats) { stats.book_tickers; };

static_assert(!HasPollDiagnostics<md::RealtimeDataReaderStats>);
static_assert(!HasOldBookTickersField<md::RealtimeDataReaderStats>);
static_assert(!HasOldBookTickersField<md::RealtimeDataReaderSourceStats>);

std::string UniqueShmName(std::string_view suffix) {
  return fmt::format("/aquila_data_reader_test_{}_{}", ::getpid(), suffix);
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

aquila::BookTicker MakeBookTicker(std::int64_t id, aquila::Exchange exchange) {
  return aquila::BookTicker{
      .id = id,
      .symbol_id = static_cast<std::int32_t>(id % 3),
      .exchange = exchange,
      .exchange_ns = 1'770'000'000'000'000'000 + id,
      .local_ns = 1'770'000'000'000'100'000 + id,
      .bid_price = 65'000.0 + static_cast<double>(id),
      .bid_volume = 10.0 + static_cast<double>(id),
      .ask_price = 65'001.0 + static_cast<double>(id),
      .ask_volume = 11.0 + static_cast<double>(id),
  };
}

cfg::DataReaderSourceConfig MakeSourceConfig(
    std::string name, aquila::Exchange exchange, std::string shm_name,
    cfg::DataReaderReadMode read_mode) {
  return cfg::DataReaderSourceConfig{
      .name = std::move(name),
      .type = cfg::DataReaderSourceType::kShm,
      .exchange = exchange,
      .feed = cfg::DataReaderFeed::kBookTicker,
      .shm_name = std::move(shm_name),
      .channel_name = "book_ticker_channel",
      .start_position = cfg::DataReaderStartPosition::kLatest,
      .read_mode = read_mode,
      .required = true,
  };
}

TEST(RealtimeDataReaderTest, PollReadsLatestBookTickerFromTwoSources) {
  const md::BookTickerShmConfig gate_config = MakeCreateConfig("gate");
  const md::BookTickerShmConfig binance_config = MakeCreateConfig("binance");
  ShmCleanup gate_cleanup(gate_config.shm_name);
  ShmCleanup binance_cleanup(binance_config.shm_name);

  md::DataShmPublisher gate_publisher(gate_config);
  md::DataShmPublisher binance_publisher(binance_config);

  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_drain = 64;
  config.sources.push_back(
      MakeSourceConfig("gate_book_ticker", aquila::Exchange::kGate,
                       gate_config.shm_name, cfg::DataReaderReadMode::kLatest));
  config.sources.push_back(MakeSourceConfig(
      "binance_book_ticker", aquila::Exchange::kBinance,
      binance_config.shm_name, cfg::DataReaderReadMode::kLatest));

  md::RealtimeDataReader reader(std::move(config));
  gate_publisher.OnBookTicker(MakeBookTicker(1, aquila::Exchange::kGate));
  binance_publisher.OnBookTicker(MakeBookTicker(2, aquila::Exchange::kBinance));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  EXPECT_EQ(handler.book_tickers[0].exchange, aquila::Exchange::kGate);

  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 2U);
  EXPECT_EQ(handler.book_tickers[1].exchange, aquila::Exchange::kBinance);
}

TEST(RealtimeDataReaderTest, PollRoundRobinsAcrossThreeSources) {
  const md::BookTickerShmConfig gate_config = MakeCreateConfig("rr_gate");
  const md::BookTickerShmConfig binance_config = MakeCreateConfig("rr_binance");
  const md::BookTickerShmConfig okx_config = MakeCreateConfig("rr_okx");
  ShmCleanup gate_cleanup(gate_config.shm_name);
  ShmCleanup binance_cleanup(binance_config.shm_name);
  ShmCleanup okx_cleanup(okx_config.shm_name);

  md::DataShmPublisher gate_publisher(gate_config);
  md::DataShmPublisher binance_publisher(binance_config);
  md::DataShmPublisher okx_publisher(okx_config);

  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_drain = 64;
  config.sources.push_back(
      MakeSourceConfig("gate_book_ticker", aquila::Exchange::kGate,
                       gate_config.shm_name, cfg::DataReaderReadMode::kLatest));
  config.sources.push_back(MakeSourceConfig(
      "binance_book_ticker", aquila::Exchange::kBinance,
      binance_config.shm_name, cfg::DataReaderReadMode::kLatest));
  config.sources.push_back(
      MakeSourceConfig("okx_book_ticker", aquila::Exchange::kOkx,
                       okx_config.shm_name, cfg::DataReaderReadMode::kLatest));

  md::RealtimeDataReader reader(std::move(config));
  gate_publisher.OnBookTicker(MakeBookTicker(1, aquila::Exchange::kGate));
  binance_publisher.OnBookTicker(MakeBookTicker(2, aquila::Exchange::kBinance));
  okx_publisher.OnBookTicker(MakeBookTicker(3, aquila::Exchange::kOkx));

  RecordingHandler handler;
  ASSERT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 3U);
  EXPECT_EQ(handler.book_tickers[0].exchange, aquila::Exchange::kGate);
  EXPECT_EQ(handler.book_tickers[1].exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(handler.book_tickers[2].exchange, aquila::Exchange::kOkx);

  gate_publisher.OnBookTicker(MakeBookTicker(4, aquila::Exchange::kGate));
  handler.book_tickers.clear();
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  EXPECT_EQ(handler.book_tickers[0].exchange, aquila::Exchange::kGate);
}

TEST(RealtimeDataReaderTest, DrainReadsAtMostMaxEvents) {
  const md::BookTickerShmConfig gate_config = MakeCreateConfig("drain_limit");
  ShmCleanup cleanup(gate_config.shm_name);
  md::DataShmPublisher publisher(gate_config);

  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_drain = 64;
  config.sources.push_back(
      MakeSourceConfig("gate_book_ticker", aquila::Exchange::kGate,
                       gate_config.shm_name, cfg::DataReaderReadMode::kDrain));

  md::RealtimeDataReader reader(std::move(config));
  publisher.OnBookTicker(MakeBookTicker(1, aquila::Exchange::kGate));
  publisher.OnBookTicker(MakeBookTicker(2, aquila::Exchange::kGate));
  publisher.OnBookTicker(MakeBookTicker(3, aquila::Exchange::kGate));

  RecordingHandler handler;
  EXPECT_EQ(reader.Drain(handler, 0), 0U);
  EXPECT_TRUE(handler.book_tickers.empty());

  EXPECT_EQ(reader.Drain(handler, 2), 2U);
  ASSERT_EQ(handler.book_tickers.size(), 2U);
  EXPECT_EQ(handler.book_tickers[0].id, 1);
  EXPECT_EQ(handler.book_tickers[1].id, 2);

  handler.book_tickers.clear();
  EXPECT_EQ(reader.Drain(handler, 10), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  EXPECT_EQ(handler.book_tickers[0].id, 3);
}

TEST(RealtimeDataReaderTest, LatestReadsOnlyLastBookTickerPerSource) {
  const md::BookTickerShmConfig binance_config =
      MakeCreateConfig("latest_only");
  ShmCleanup cleanup(binance_config.shm_name);
  md::DataShmPublisher publisher(binance_config);

  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_drain = 64;
  config.sources.push_back(MakeSourceConfig(
      "binance_book_ticker", aquila::Exchange::kBinance,
      binance_config.shm_name, cfg::DataReaderReadMode::kLatest));

  md::RealtimeDataReader reader(std::move(config));
  publisher.OnBookTicker(MakeBookTicker(10, aquila::Exchange::kBinance));
  publisher.OnBookTicker(MakeBookTicker(11, aquila::Exchange::kBinance));
  publisher.OnBookTicker(MakeBookTicker(12, aquila::Exchange::kBinance));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  EXPECT_EQ(handler.book_tickers[0].id, 12);

  handler.book_tickers.clear();
  EXPECT_EQ(reader.Poll(handler), 0U);
  EXPECT_TRUE(handler.book_tickers.empty());
}

TEST(RealtimeDataReaderTest, DiagnosticsTrackBookTickersAndSkippedCounts) {
  using Reader = md::RealtimeDataReader<md::RealtimeDataReaderDiagnostics>;

  const md::BookTickerShmConfig config = MakeCreateConfig("diag_latest");
  ShmCleanup cleanup(config.shm_name);
  md::DataShmPublisher publisher(config);

  cfg::DataReaderConfig reader_config;
  reader_config.name = "test_data_reader";
  reader_config.max_events_per_drain = 64;
  reader_config.sources.push_back(
      MakeSourceConfig("gate_book_ticker", aquila::Exchange::kGate,
                       config.shm_name, cfg::DataReaderReadMode::kLatest));

  Reader reader(std::move(reader_config));
  publisher.OnBookTicker(MakeBookTicker(1, aquila::Exchange::kGate));
  publisher.OnBookTicker(MakeBookTicker(2, aquila::Exchange::kGate));
  publisher.OnBookTicker(MakeBookTicker(3, aquila::Exchange::kGate));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  EXPECT_EQ(reader.Poll(handler), 0U);

  const md::RealtimeDataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.total_count, 1U);
  ASSERT_EQ(stats.sources.size(), 1U);
  EXPECT_EQ(stats.sources[0].book_ticker_count, 1U);
  EXPECT_EQ(stats.sources[0].skipped, 2U);
  EXPECT_EQ(stats.sources[0].last_book_ticker_id, 3);
}

TEST(RealtimeDataReaderTest, RejectsEmptySources) {
  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_drain = 64;

  EXPECT_THROW((void)md::RealtimeDataReader<>(std::move(config)),
               std::invalid_argument);
}

TEST(RealtimeDataReaderTest, RejectsNonShmSource) {
  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_drain = 64;
  config.sources.push_back(MakeSourceConfig("bad_source",
                                            aquila::Exchange::kGate, "/unused",
                                            cfg::DataReaderReadMode::kLatest));
  config.sources.back().type = cfg::DataReaderSourceType::kBinaryFile;

  EXPECT_THROW((void)md::RealtimeDataReader<>(std::move(config)),
               std::invalid_argument);
}

TEST(RealtimeDataReaderTest, RejectsInvalidFeed) {
  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_drain = 64;
  config.sources.push_back(MakeSourceConfig("bad_feed", aquila::Exchange::kGate,
                                            "/unused",
                                            cfg::DataReaderReadMode::kLatest));
  config.sources.back().feed = static_cast<cfg::DataReaderFeed>(255);

  EXPECT_THROW((void)md::RealtimeDataReader<>(std::move(config)),
               std::invalid_argument);
}

TEST(RealtimeDataReaderTest, RejectsInvalidStartPosition) {
  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_drain = 64;
  config.sources.push_back(MakeSourceConfig("bad_start_position",
                                            aquila::Exchange::kGate, "/unused",
                                            cfg::DataReaderReadMode::kLatest));
  config.sources.back().start_position =
      static_cast<cfg::DataReaderStartPosition>(255);

  EXPECT_THROW((void)md::RealtimeDataReader<>(std::move(config)),
               std::invalid_argument);
}

TEST(RealtimeDataReaderTest, RejectsInvalidReadMode) {
  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_drain = 64;
  config.sources.push_back(MakeSourceConfig("bad_read_mode",
                                            aquila::Exchange::kGate, "/unused",
                                            cfg::DataReaderReadMode::kLatest));
  config.sources.back().read_mode = static_cast<cfg::DataReaderReadMode>(255);

  EXPECT_THROW((void)md::RealtimeDataReader<>(std::move(config)),
               std::invalid_argument);
}

}  // namespace
