#include "core/market_data/data_reader.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

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

TEST(DataReaderTest, PollReadsLatestBookTickerFromTwoSources) {
  const md::BookTickerShmConfig gate_config = MakeCreateConfig("gate");
  const md::BookTickerShmConfig binance_config = MakeCreateConfig("binance");
  ShmCleanup gate_cleanup(gate_config.shm_name);
  ShmCleanup binance_cleanup(binance_config.shm_name);

  md::DataShmPublisher gate_publisher(gate_config);
  md::DataShmPublisher binance_publisher(binance_config);

  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_source = 64;
  config.sources.push_back(
      MakeSourceConfig("gate_book_ticker", aquila::Exchange::kGate,
                       gate_config.shm_name, cfg::DataReaderReadMode::kLatest));
  config.sources.push_back(MakeSourceConfig(
      "binance_book_ticker", aquila::Exchange::kBinance,
      binance_config.shm_name, cfg::DataReaderReadMode::kLatest));

  md::DataReader reader(std::move(config));
  gate_publisher.OnBookTicker(MakeBookTicker(1, aquila::Exchange::kGate));
  binance_publisher.OnBookTicker(MakeBookTicker(2, aquila::Exchange::kBinance));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 2U);
  ASSERT_EQ(handler.book_tickers.size(), 2U);
  EXPECT_EQ(handler.book_tickers[0].exchange, aquila::Exchange::kGate);
  EXPECT_EQ(handler.book_tickers[1].exchange, aquila::Exchange::kBinance);
}

TEST(DataReaderTest, DrainReadsAtMostMaxEventsPerSource) {
  const md::BookTickerShmConfig gate_config = MakeCreateConfig("drain_limit");
  ShmCleanup cleanup(gate_config.shm_name);
  md::DataShmPublisher publisher(gate_config);

  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_source = 2;
  config.sources.push_back(
      MakeSourceConfig("gate_book_ticker", aquila::Exchange::kGate,
                       gate_config.shm_name, cfg::DataReaderReadMode::kDrain));

  md::DataReader reader(std::move(config));
  publisher.OnBookTicker(MakeBookTicker(1, aquila::Exchange::kGate));
  publisher.OnBookTicker(MakeBookTicker(2, aquila::Exchange::kGate));
  publisher.OnBookTicker(MakeBookTicker(3, aquila::Exchange::kGate));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 2U);
  ASSERT_EQ(handler.book_tickers.size(), 2U);
  EXPECT_EQ(handler.book_tickers[0].id, 1);
  EXPECT_EQ(handler.book_tickers[1].id, 2);

  handler.book_tickers.clear();
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  EXPECT_EQ(handler.book_tickers[0].id, 3);
}

TEST(DataReaderTest, LatestReadsOnlyLastBookTickerPerSource) {
  const md::BookTickerShmConfig binance_config =
      MakeCreateConfig("latest_only");
  ShmCleanup cleanup(binance_config.shm_name);
  md::DataShmPublisher publisher(binance_config);

  cfg::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_source = 64;
  config.sources.push_back(MakeSourceConfig(
      "binance_book_ticker", aquila::Exchange::kBinance,
      binance_config.shm_name, cfg::DataReaderReadMode::kLatest));

  md::DataReader reader(std::move(config));
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

}  // namespace
