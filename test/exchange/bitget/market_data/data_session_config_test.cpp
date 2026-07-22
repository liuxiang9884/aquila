#include "exchange/bitget/market_data/data_session_config.h"

#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

std::string BaseToml(std::string_view feeds) {
  return std::string{R"toml(
[instrument_catalog]
file = ")toml"} +
         SourcePath("config/instruments/usdt_future_universe.csv").string() +
         R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "bitget_data_session"
inst_type = "usdt-futures"
subscribe_symbols = ["BTCUSDT", "ETHUSDT"]
)toml" + std::string{feeds} +
         R"toml(

[data_session.websocket.endpoint]
host = "vip-ws-uta.bitget.com"
target = "/v3/ws/public/sbe"
enable_tls = true

[data_session.websocket.execution_policy]
bind_cpu_id = 2

[data_shm_sink]
enabled = true
shm_name = "aquila_bitget_market_data"
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
create = true
remove_existing = false
)toml";
}

}  // namespace

TEST(BitgetDataSessionConfigTest, ParsesBookTickerConfig) {
  const toml::parse_result parsed =
      toml::parse(BaseToml(R"toml(feeds = ["book_ticker"])toml"));

  const auto result = aquila::bitget::ParseDataSessionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::bitget::DataSessionConfig& config = result.value;
  EXPECT_EQ(config.name, "bitget_data_session");
  EXPECT_EQ(config.inst_type, "usdt-futures");
  EXPECT_TRUE(config.feeds.book_ticker);
  EXPECT_FALSE(config.feeds.trade);
  EXPECT_TRUE(config.data_shm.book_ticker_enabled);
  EXPECT_FALSE(config.data_shm.trade_enabled);
  EXPECT_EQ(config.connection.host, "vip-ws-uta.bitget.com");
  EXPECT_EQ(config.connection.target, "/v3/ws/public/sbe");
  EXPECT_TRUE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 2);
  EXPECT_EQ(config.data_shm.shm_name, "aquila_bitget_market_data");
  EXPECT_EQ(config.data_shm.book_ticker_channel_name, "book_ticker_channel");
  ASSERT_EQ(config.exchange_symbols.size(), 2u);
  EXPECT_EQ(config.exchange_symbols[0], "BTCUSDT");
  EXPECT_EQ(config.exchange_symbols[1], "ETHUSDT");
  ASSERT_EQ(config.symbol_ids.size(), 2u);
  EXPECT_EQ(config.symbol_ids[0], 93);
  EXPECT_EQ(config.symbol_ids[1], 163);
}

TEST(BitgetDataSessionConfigTest, ParsesBookTickerAndTradeConfig) {
  const toml::parse_result parsed =
      toml::parse(BaseToml(R"toml(feeds = ["book_ticker", "trade"])toml"));

  const auto result = aquila::bitget::ParseDataSessionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::bitget::DataSessionConfig& config = result.value;
  EXPECT_TRUE(config.feeds.book_ticker);
  EXPECT_TRUE(config.feeds.trade);
  EXPECT_TRUE(config.data_shm.book_ticker_enabled);
  EXPECT_TRUE(config.data_shm.trade_enabled);
  EXPECT_EQ(config.data_shm.book_ticker_channel_name, "book_ticker_channel");
  EXPECT_EQ(config.data_shm.trade_channel_name, "trade_channel");
}

TEST(BitgetDataSessionConfigTest, ParsesTradeOnlyConfig) {
  const toml::parse_result parsed =
      toml::parse(BaseToml(R"toml(feeds = ["trade"])toml"));

  const auto result = aquila::bitget::ParseDataSessionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::bitget::DataSessionConfig& config = result.value;
  EXPECT_FALSE(config.feeds.book_ticker);
  EXPECT_TRUE(config.feeds.trade);
  EXPECT_FALSE(config.data_shm.book_ticker_enabled);
  EXPECT_TRUE(config.data_shm.trade_enabled);
}

TEST(BitgetDataSessionConfigTest, RejectsDuplicateFeeds) {
  const toml::parse_result parsed = toml::parse(
      BaseToml(R"toml(feeds = ["book_ticker", "book_ticker"])toml"));

  const auto result = aquila::bitget::ParseDataSessionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("duplicate feed book_ticker"), std::string::npos);
}

TEST(BitgetDataSessionConfigTest, RejectsUnknownFeed) {
  const toml::parse_result parsed =
      toml::parse(BaseToml(R"toml(feeds = ["depth"])toml"));

  const auto result = aquila::bitget::ParseDataSessionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("unknown Bitget data_session feed"),
            std::string::npos);
}

TEST(BitgetDataSessionConfigTest, RejectsDuplicateTradeFeeds) {
  const toml::parse_result parsed =
      toml::parse(BaseToml(R"toml(feeds = ["trade", "trade"])toml"));

  const auto result = aquila::bitget::ParseDataSessionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("duplicate feed trade"), std::string::npos);
}

TEST(BitgetDataSessionConfigTest, RejectsMissingSubscribeSymbols) {
  const std::string toml_text = std::string{R"toml(
[instrument_catalog]
file = ")toml"} + SourcePath("config/instruments/usdt_future_universe.csv").string() +
                                R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "bitget_data_session"
inst_type = "usdt-futures"
feeds = ["book_ticker"]

[data_session.websocket.endpoint]
host = "vip-ws-uta.bitget.com"

[data_session.websocket.execution_policy]
bind_cpu_id = 2
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::bitget::ParseDataSessionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_session.subscribe_symbols"),
            std::string::npos);
}

TEST(BitgetDataSessionConfigTest, RejectsInvalidBookTickerChannelName) {
  std::string toml_text = BaseToml(R"toml(feeds = ["book_ticker"])toml");
  const std::string needle =
      "book_ticker_channel_name = \"book_ticker_channel\"";
  const std::size_t pos = toml_text.find(needle);
  ASSERT_NE(pos, std::string::npos);
  toml_text.replace(pos, needle.size(), "book_ticker_channel_name = \"\"");

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::bitget::ParseDataSessionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_shm_sink.book_ticker_channel_name"),
            std::string::npos);
}

TEST(BitgetDataSessionConfigTest, RejectsInvalidTradeChannelName) {
  std::string toml_text = BaseToml(R"toml(feeds = ["trade"])toml");
  const std::string needle = "trade_channel_name = \"trade_channel\"";
  const std::size_t pos = toml_text.find(needle);
  ASSERT_NE(pos, std::string::npos);
  toml_text.replace(pos, needle.size(), "trade_channel_name = \"\"");

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::bitget::ParseDataSessionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_shm_sink.trade_channel_name"),
            std::string::npos);
}
