#include "core/config/data_reader_config.h"

#include <filesystem>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <toml++/toml.hpp>

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

std::string CatalogPrefix() {
  return std::string{R"toml(
[instrument_catalog]
file = ")toml"} +
         SourcePath("config/instruments/usdt_futures.csv").string() +
         R"toml("
schema = "aquila.instrument.v1"

)toml";
}

TEST(DataReaderConfigTest, LoadsReadyStrategyDataReaderConfig) {
  const auto result = aquila::config::LoadDataReaderConfigFile(
      SourcePath("config/data_readers/strategy_data_reader.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::DataReaderConfig& config = result.value;

  EXPECT_EQ(config.name, "strategy_data_reader");
  EXPECT_EQ(config.max_events_per_drain, 64U);
  EXPECT_EQ(config.execution_policy.bind_cpu_id, 4);
  EXPECT_EQ(config.execution_policy.idle_policy, "spin");

  ASSERT_EQ(config.sources.size(), 2U);
  EXPECT_EQ(config.sources[0].name, "gate_book_ticker");
  EXPECT_EQ(config.sources[0].type, aquila::config::DataReaderSourceType::kShm);
  EXPECT_EQ(config.sources[0].exchange, aquila::Exchange::kGate);
  EXPECT_EQ(config.sources[0].feed,
            aquila::config::DataReaderFeed::kBookTicker);
  EXPECT_EQ(config.sources[0].shm_name, "aquila_gate_market_data");
  EXPECT_EQ(config.sources[0].channel_name, "book_ticker_channel");
  EXPECT_EQ(config.sources[0].start_position,
            aquila::config::DataReaderStartPosition::kLatest);
  EXPECT_EQ(config.sources[0].read_mode,
            aquila::config::DataReaderReadMode::kLatest);
  EXPECT_TRUE(config.sources[0].required);

  EXPECT_EQ(config.sources[1].name, "binance_book_ticker");
  EXPECT_EQ(config.sources[1].exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(config.sources[1].shm_name, "aquila_binance_market_data_combined");
  EXPECT_EQ(config.sources[1].read_mode,
            aquila::config::DataReaderReadMode::kLatest);
}

TEST(DataReaderConfigTest, LoadsReadyFirst5StrategyDataReaderConfig) {
  const auto result = aquila::config::LoadDataReaderConfigFile(SourcePath(
      "config/data_readers/strategy_data_reader_first5_20260521.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::DataReaderConfig& config = result.value;

  EXPECT_EQ(config.name, "strategy_data_reader_first5");
  EXPECT_EQ(config.max_events_per_drain, 64U);
  ASSERT_EQ(config.sources.size(), 2U);
  EXPECT_EQ(config.sources[0].name, "gate_book_ticker_first5");
  EXPECT_EQ(config.sources[0].shm_name,
            "aquila_gate_market_data_first5_20260521");
  EXPECT_EQ(config.sources[1].name, "binance_book_ticker_first5");
  EXPECT_EQ(config.sources[1].shm_name,
            "aquila_binance_market_data_first5_20260521");
}

TEST(DataReaderConfigTest, LoadsReadyRequestedStrategyDataReaderConfig) {
  const auto result = aquila::config::LoadDataReaderConfigFile(SourcePath(
      "config/data_readers/strategy_data_reader_requested_20260521.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::DataReaderConfig& config = result.value;

  EXPECT_EQ(config.name, "strategy_data_reader_requested");
  EXPECT_EQ(config.max_events_per_drain, 128U);
  ASSERT_EQ(config.sources.size(), 2U);
  EXPECT_EQ(config.sources[0].name, "gate_book_ticker_requested");
  EXPECT_EQ(config.sources[0].shm_name,
            "aquila_gate_market_data_requested_20260521");
  EXPECT_EQ(config.sources[1].name, "binance_book_ticker_requested");
  EXPECT_EQ(config.sources[1].shm_name,
            "aquila_binance_market_data_requested_20260521");
}

TEST(DataReaderConfigTest, LoadsReadyLabUsdtStrategyDataReaderConfig) {
  const auto result = aquila::config::LoadDataReaderConfigFile(SourcePath(
      "config/data_readers/strategy_data_reader_lab_usdt_20260601.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::DataReaderConfig& config = result.value;

  EXPECT_EQ(config.name, "strategy_data_reader_lab_usdt_20260601");
  EXPECT_EQ(config.max_events_per_drain, 128U);
  EXPECT_EQ(config.execution_policy.bind_cpu_id, 4);
  EXPECT_EQ(config.execution_policy.idle_policy, "spin");

  ASSERT_EQ(config.sources.size(), 2U);
  EXPECT_EQ(config.sources[0].name, "gate_book_ticker_lab_usdt_20260601");
  EXPECT_EQ(config.sources[0].exchange, aquila::Exchange::kGate);
  EXPECT_EQ(config.sources[0].shm_name,
            "aquila_gate_market_data_lab_usdt_20260601");
  EXPECT_EQ(config.sources[0].read_mode,
            aquila::config::DataReaderReadMode::kLatest);
  EXPECT_TRUE(config.sources[0].required);

  EXPECT_EQ(config.sources[1].name, "binance_book_ticker_lab_usdt_20260601");
  EXPECT_EQ(config.sources[1].exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(config.sources[1].shm_name,
            "aquila_binance_market_data_lab_usdt_20260601");
  EXPECT_EQ(config.sources[1].read_mode,
            aquila::config::DataReaderReadMode::kLatest);
  EXPECT_TRUE(config.sources[1].required);
}

TEST(DataReaderConfigTest, ParsesBitgetShmSource) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "bitget_reader"
max_events_per_drain = 128

[[data_reader.sources]]
name = "bitget_book_ticker"
type = "shm"
exchange = "bitget"
feed = "book_ticker"
shm_name = "aquila_bitget_book_ticker_fusion"
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "drain"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.sources.size(), 1U);
  const aquila::config::DataReaderSourceConfig& source =
      result.value.sources[0];
  EXPECT_EQ(source.name, "bitget_book_ticker");
  EXPECT_EQ(source.exchange, aquila::Exchange::kBitget);
  EXPECT_EQ(source.feed, aquila::config::DataReaderFeed::kBookTicker);
  EXPECT_EQ(source.shm_name, "aquila_bitget_book_ticker_fusion");
  EXPECT_EQ(source.channel_name, "book_ticker_channel");
  EXPECT_EQ(source.read_mode, aquila::config::DataReaderReadMode::kDrain);
}

TEST(DataReaderConfigTest, ParsesBitgetTradeShmSource) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "bitget_trade_reader"
max_events_per_drain = 128

[[data_reader.sources]]
name = "bitget_trade"
type = "shm"
exchange = "bitget"
feed = "trade"
shm_name = "aquila_bitget_trade_fusion"
channel_name = "trade_channel"
start_position = "latest"
read_mode = "drain"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.sources.size(), 1U);
  const aquila::config::DataReaderSourceConfig& source =
      result.value.sources[0];
  EXPECT_EQ(source.name, "bitget_trade");
  EXPECT_EQ(source.exchange, aquila::Exchange::kBitget);
  EXPECT_EQ(source.feed, aquila::config::DataReaderFeed::kTrade);
  EXPECT_EQ(source.shm_name, "aquila_bitget_trade_fusion");
  EXPECT_EQ(source.channel_name, "trade_channel");
  EXPECT_EQ(source.read_mode, aquila::config::DataReaderReadMode::kDrain);
}

TEST(DataReaderConfigTest, RejectsDuplicateSourceNames) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "strategy_data_reader"
max_events_per_drain = 64

[[data_reader.sources]]
name = "dup"
type = "shm"
exchange = "gate"
feed = "book_ticker"
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"

[[data_reader.sources]]
name = "dup"
type = "shm"
exchange = "binance"
feed = "book_ticker"
shm_name = "aquila_binance_market_data"
channel_name = "book_ticker_channel"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.sources.name"), std::string::npos);
}

TEST(DataReaderConfigTest, RejectsUnsupportedReadMode) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "strategy_data_reader"

[[data_reader.sources]]
name = "gate_book_ticker"
type = "shm"
exchange = "gate"
feed = "book_ticker"
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
read_mode = "all"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("read_mode"), std::string::npos);
}

TEST(DataReaderConfigTest, ParsesBinaryFileSource) {
  const std::filesystem::path first_file = SourcePath("tmp/book_ticker_1.bin");
  const std::filesystem::path second_file = SourcePath("tmp/book_ticker_2.bin");
  const std::string toml_text = fmt::format(
      R"toml({}
[data_reader]
name = "binary_replay_reader"
max_events_per_drain = 8

[[data_reader.sources]]
name = "binary_book_ticker"
type = "binary_file"
feed = "book_ticker"
files = ["{}", "{}"]
start_position = "earliest_visible"
read_mode = "drain"
)toml",
      CatalogPrefix(), first_file.string(), second_file.string());

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_TRUE(result.ok) << result.error;

  ASSERT_EQ(result.value.sources.size(), 1U);
  const aquila::config::DataReaderSourceConfig& source =
      result.value.sources[0];
  EXPECT_EQ(source.name, "binary_book_ticker");
  EXPECT_EQ(source.type, aquila::config::DataReaderSourceType::kBinaryFile);
  EXPECT_EQ(source.feed, aquila::config::DataReaderFeed::kBookTicker);
  EXPECT_TRUE(source.shm_name.empty());
  EXPECT_TRUE(source.channel_name.empty());
  EXPECT_EQ(source.start_position,
            aquila::config::DataReaderStartPosition::kEarliestVisible);
  EXPECT_EQ(source.read_mode, aquila::config::DataReaderReadMode::kDrain);
  ASSERT_EQ(source.files.size(), 2U);
  EXPECT_EQ(source.files[0], first_file);
  EXPECT_EQ(source.files[1], second_file);
}

TEST(DataReaderConfigTest, RejectsBinaryFileWithoutExplicitFeed) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "binary_reader"

[[data_reader.sources]]
name = "recorded"
type = "binary_file"
files = ["recorded.bin"]
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.sources.feed"), std::string::npos);
}

TEST(DataReaderConfigTest, RejectsBinaryFileNonStringFeed) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "binary_reader"

[[data_reader.sources]]
name = "recorded"
type = "binary_file"
feed = 17
files = ["recorded.bin"]
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.sources.feed"), std::string::npos);
}

TEST(DataReaderConfigTest, RejectsBinaryFileEmptyFeed) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "binary_reader"

[[data_reader.sources]]
name = "recorded"
type = "binary_file"
feed = ""
files = ["recorded.bin"]
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.sources.feed"), std::string::npos);
}

TEST(DataReaderConfigTest, RejectsBinaryFileBadFeed) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "binary_reader"

[[data_reader.sources]]
name = "recorded"
type = "binary_file"
feed = "bad"
files = ["recorded.bin"]
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.sources.feed"), std::string::npos);
}

TEST(DataReaderConfigTest, ShmSourceWithoutFeedDefaultsToBookTicker) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "live_reader"

[[data_reader.sources]]
name = "gate"
type = "shm"
exchange = "gate"
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.sources.size(), 1U);
  EXPECT_EQ(result.value.sources[0].feed,
            aquila::config::DataReaderFeed::kBookTicker);
}

TEST(DataReaderConfigTest, ParsesTradeShmSource) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "trade_reader"
max_events_per_drain = 8

[[data_reader.sources]]
name = "gate_trade"
type = "shm"
exchange = "gate"
feed = "trade"
shm_name = "aquila_gate_market_data"
channel_name = "trade_channel"
start_position = "earliest_visible"
read_mode = "drain"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_TRUE(result.ok) << result.error;

  ASSERT_EQ(result.value.sources.size(), 1U);
  const aquila::config::DataReaderSourceConfig& source =
      result.value.sources[0];
  EXPECT_EQ(source.name, "gate_trade");
  EXPECT_EQ(source.type, aquila::config::DataReaderSourceType::kShm);
  EXPECT_EQ(source.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(source.feed, aquila::config::DataReaderFeed::kTrade);
  EXPECT_EQ(source.shm_name, "aquila_gate_market_data");
  EXPECT_EQ(source.channel_name, "trade_channel");
  EXPECT_EQ(source.start_position,
            aquila::config::DataReaderStartPosition::kEarliestVisible);
  EXPECT_EQ(source.read_mode, aquila::config::DataReaderReadMode::kDrain);
  EXPECT_TRUE(source.required);
}

TEST(DataReaderConfigTest, ParsesTradeBinaryFileSource) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "binary_trade_reader"

[[data_reader.sources]]
name = "binary_trade"
type = "binary_file"
feed = "trade"
files = ["/home/liuxiang/tmp/trade.bin"]
start_position = "earliest_visible"
read_mode = "drain"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_TRUE(result.ok) << result.error;

  ASSERT_EQ(result.value.sources.size(), 1U);
  const aquila::config::DataReaderSourceConfig& source =
      result.value.sources[0];
  EXPECT_EQ(source.name, "binary_trade");
  EXPECT_EQ(source.type, aquila::config::DataReaderSourceType::kBinaryFile);
  EXPECT_EQ(source.feed, aquila::config::DataReaderFeed::kTrade);
  ASSERT_EQ(source.files.size(), 1U);
  EXPECT_EQ(source.files[0],
            std::filesystem::path{"/home/liuxiang/tmp/trade.bin"});
}

TEST(DataReaderConfigTest, RejectsBinaryFileWithoutFiles) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "binary_replay_reader"

[[data_reader.sources]]
name = "binary_book_ticker"
type = "binary_file"
feed = "book_ticker"
read_mode = "drain"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("files"), std::string::npos);
}

TEST(DataReaderConfigTest, RejectsBinaryFileLatestReadMode) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "binary_replay_reader"

[[data_reader.sources]]
name = "binary_book_ticker"
type = "binary_file"
feed = "book_ticker"
files = ["/tmp/book_ticker.bin"]
read_mode = "latest"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("read_mode"), std::string::npos);
}

TEST(DataReaderConfigTest, RejectsBinaryFileLatestStartPosition) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "binary_replay_reader"

[[data_reader.sources]]
name = "binary_book_ticker"
type = "binary_file"
feed = "book_ticker"
files = ["/tmp/book_ticker.bin"]
start_position = "latest"
read_mode = "drain"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("start_position"), std::string::npos);
}

TEST(DataReaderConfigTest, RejectsMissingShmNameForShmSource) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "strategy_data_reader"

[[data_reader.sources]]
name = "gate_book_ticker"
type = "shm"
exchange = "gate"
feed = "book_ticker"
channel_name = "book_ticker_channel"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("shm_name"), std::string::npos);
}

TEST(DataReaderConfigTest, RejectsZeroMaxEventsPerDrain) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "strategy_data_reader"
max_events_per_drain = 0

[[data_reader.sources]]
name = "gate_book_ticker"
type = "shm"
exchange = "gate"
feed = "book_ticker"
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.max_events_per_drain"),
            std::string::npos);
}

TEST(DataReaderConfigTest, RejectsOverflowMaxEventsPerDrain) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "strategy_data_reader"
max_events_per_drain = 4294967296

[[data_reader.sources]]
name = "gate_book_ticker"
type = "shm"
exchange = "gate"
feed = "book_ticker"
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.max_events_per_drain"),
            std::string::npos);
}

TEST(DataReaderConfigTest, RejectsLegacyMaxEventsPerSourceField) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "strategy_data_reader"
max_events_per_source = 64

[[data_reader.sources]]
name = "gate_book_ticker"
type = "shm"
exchange = "gate"
feed = "book_ticker"
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.max_events_per_source"),
            std::string::npos);
  EXPECT_NE(result.error.find("data_reader.max_events_per_drain"),
            std::string::npos);
}

TEST(DataReaderConfigTest, RejectsEmptySources) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "strategy_data_reader"
max_events_per_drain = 64
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.sources"), std::string::npos);
}

}  // namespace
