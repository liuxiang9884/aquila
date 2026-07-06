#include "tools/binance/binance_data_fusion_config.h"

#include <string>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

namespace {

namespace support = aquila::tools::market_data;

toml::parse_result ParseToml(const std::string& text) {
  return toml::parse(text);
}

TEST(BinanceDataFusionConfigTest, ParsesBookTickerSources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_book_ticker_4sources"
fusion_config = "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session_30symbols_20260604.toml"
data_session_name = "binance_source_0"
book_ticker_shm_name = "aquila_binance_book_ticker_src_0"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 16

[[launch.sources]]
source_id = 1
data_session_config = "config/data_sessions/binance_data_session_30symbols_20260604.toml"
data_session_name = "binance_source_1"
book_ticker_shm_name = "aquila_binance_book_ticker_src_1"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 17
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "binance_data_fusion_book_ticker_4sources");
  EXPECT_EQ(result.value.feed, support::DataFusionFeed::kBookTicker);
  EXPECT_EQ(
      result.value.fusion_config,
      "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml");
  ASSERT_EQ(result.value.sources.size(), 2U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].data_session_name, "binance_source_0");
  EXPECT_EQ(result.value.sources[0].book_ticker_shm_name,
            "aquila_binance_book_ticker_src_0");
  EXPECT_EQ(result.value.sources[0].book_ticker_channel_name,
            "book_ticker_channel");
  EXPECT_TRUE(result.value.sources[0].remove_existing_source_shm);
  EXPECT_EQ(result.value.sources[0].bind_cpu_id, 16);
  EXPECT_EQ(result.value.sources[1].source_id, 1);
  EXPECT_EQ(result.value.sources[1].bind_cpu_id, 17);
}

TEST(BinanceDataFusionConfigTest, ParsesTradeSources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_trade_4sources"
feed = "trade"
fusion_config = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session_30symbols_20260604.toml"
data_session_name = "binance_trade_source_0"
trade_shm_name = "aquila_binance_trade_src_0"
trade_channel_name = "trade_channel"
remove_existing_source_shm = true
bind_cpu_id = 17
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "binance_data_fusion_trade_4sources");
  EXPECT_EQ(result.value.feed, support::DataFusionFeed::kTrade);
  EXPECT_EQ(
      result.value.fusion_config,
      "config/market_data_fusion/binance_trade_fusion_4sources.toml");
  ASSERT_EQ(result.value.sources.size(), 1U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].data_session_name,
            "binance_trade_source_0");
  EXPECT_EQ(result.value.sources[0].trade_shm_name,
            "aquila_binance_trade_src_0");
  EXPECT_EQ(result.value.sources[0].trade_channel_name, "trade_channel");
  EXPECT_TRUE(result.value.sources[0].remove_existing_source_shm);
  EXPECT_EQ(result.value.sources[0].bind_cpu_id, 17);
}

TEST(BinanceDataFusionConfigTest, RejectsUnknownFeed) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_unknown"
feed = "depth"
fusion_config = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_0"
trade_shm_name = "aquila_binance_trade_src_0"
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("launch.feed"), std::string::npos);
}

TEST(BinanceDataFusionConfigTest, RejectsDuplicateSourceId) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_book_ticker"
fusion_config = "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 1
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_1a"
book_ticker_shm_name = "aquila_binance_book_ticker_src_1a"

[[launch.sources]]
source_id = 1
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_1b"
book_ticker_shm_name = "aquila_binance_book_ticker_src_1b"
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("source_id"), std::string::npos);
}

}  // namespace
