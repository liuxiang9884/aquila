#include "core/config/data_reader_config.h"

#include <filesystem>
#include <string>
#include <string_view>

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
  EXPECT_EQ(config.max_events_per_source, 64U);
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
  EXPECT_EQ(config.sources[1].shm_name, "aquila_binance_market_data");
  EXPECT_EQ(config.sources[1].read_mode,
            aquila::config::DataReaderReadMode::kLatest);
}

TEST(DataReaderConfigTest, RejectsDuplicateSourceNames) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "strategy_data_reader"
max_events_per_source = 64

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

TEST(DataReaderConfigTest, RejectsZeroMaxEventsPerSource) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "strategy_data_reader"
max_events_per_source = 0

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
}

TEST(DataReaderConfigTest, RejectsEmptySources) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "strategy_data_reader"
max_events_per_source = 64
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.sources"), std::string::npos);
}

}  // namespace
