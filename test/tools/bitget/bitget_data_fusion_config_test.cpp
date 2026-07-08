#include "tools/bitget/bitget_data_fusion_config.h"

#include <string>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

namespace {

namespace support = aquila::tools::market_data;

toml::parse_result ParseToml(const std::string& text) {
  return toml::parse(text);
}

TEST(BitgetDataFusionConfigTest, ParsesBookTickerSources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "bitget_data_fusion_book_ticker_4sources"
feeds = ["book_ticker"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_0"
data_shm_name = "aquila_bitget_book_ticker_src_0"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 17

[[launch.sources]]
source_id = 1
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_1"
data_shm_name = "aquila_bitget_book_ticker_src_1"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 18
)toml");

  const auto result =
      aquila::tools::bitget::ParseBitgetDataFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "bitget_data_fusion_book_ticker_4sources");
  ASSERT_EQ(result.value.feeds.size(), 1U);
  EXPECT_EQ(result.value.feeds[0], support::DataFusionFeed::kBookTicker);
  EXPECT_EQ(
      result.value.book_ticker_fusion_config,
      "config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml");
  EXPECT_TRUE(result.value.trade_fusion_config.empty());
  ASSERT_EQ(result.value.sources.size(), 2U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].data_session_name, "bitget_source_0");
  EXPECT_EQ(result.value.sources[0].data_shm_name,
            "aquila_bitget_book_ticker_src_0");
  EXPECT_EQ(result.value.sources[0].book_ticker_channel_name,
            "book_ticker_channel");
  EXPECT_TRUE(result.value.sources[0].remove_existing_source_shm);
  EXPECT_EQ(result.value.sources[0].bind_cpu_id, 17);
  EXPECT_EQ(result.value.sources[1].source_id, 1);
  EXPECT_EQ(result.value.sources[1].bind_cpu_id, 18);
}

TEST(BitgetDataFusionConfigTest, ParsesTradeSources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "bitget_data_fusion_trade"
feeds = ["trade"]

[launch.fusion_configs]
trade = "config/market_data_fusion/bitget_trade_fusion.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_0"
data_shm_name = "aquila_bitget_trade_src_0"
trade_channel_name = "trade_channel"
)toml");

  const auto result =
      aquila::tools::bitget::ParseBitgetDataFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "bitget_data_fusion_trade");
  ASSERT_EQ(result.value.feeds.size(), 1U);
  EXPECT_EQ(result.value.feeds[0], support::DataFusionFeed::kTrade);
  EXPECT_TRUE(result.value.book_ticker_fusion_config.empty());
  EXPECT_EQ(result.value.trade_fusion_config,
            "config/market_data_fusion/bitget_trade_fusion.toml");
  ASSERT_EQ(result.value.sources.size(), 1U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].trade_channel_name, "trade_channel");
}

TEST(BitgetDataFusionConfigTest, ParsesBookTickerAndTradeSources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "bitget_data_fusion_book_ticker_trade"
feeds = ["book_ticker", "trade"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/bitget_book_ticker_fusion.toml"
trade = "config/market_data_fusion/bitget_trade_fusion.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_0"
data_shm_name = "aquila_bitget_md_src_0"
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
)toml");

  const auto result =
      aquila::tools::bitget::ParseBitgetDataFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "bitget_data_fusion_book_ticker_trade");
  ASSERT_EQ(result.value.feeds.size(), 2U);
  EXPECT_EQ(result.value.feeds[0], support::DataFusionFeed::kBookTicker);
  EXPECT_EQ(result.value.feeds[1], support::DataFusionFeed::kTrade);
  EXPECT_EQ(result.value.book_ticker_fusion_config,
            "config/market_data_fusion/bitget_book_ticker_fusion.toml");
  EXPECT_EQ(result.value.trade_fusion_config,
            "config/market_data_fusion/bitget_trade_fusion.toml");
}

TEST(BitgetDataFusionConfigTest, RejectsDuplicateFeed) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "bitget_data_fusion_duplicate_feed"
feeds = ["book_ticker", "book_ticker"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_0"
data_shm_name = "aquila_bitget_book_ticker_src_0"
)toml");

  const auto result =
      aquila::tools::bitget::ParseBitgetDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("duplicate feed book_ticker"), std::string::npos);
}

TEST(BitgetDataFusionConfigTest, RejectsMissingBookTickerFusionConfig) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "bitget_data_fusion_missing_book_ticker_config"
feeds = ["book_ticker"]

[launch.fusion_configs]

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_0"
data_shm_name = "aquila_bitget_book_ticker_src_0"
)toml");

  const auto result =
      aquila::tools::bitget::ParseBitgetDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("launch.fusion_configs.book_ticker"),
            std::string::npos);
}

TEST(BitgetDataFusionConfigTest, RejectsDuplicateSourceId) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "bitget_data_fusion_duplicate_source_id"
feeds = ["book_ticker"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_0"
data_shm_name = "aquila_bitget_book_ticker_src_0"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_1"
data_shm_name = "aquila_bitget_book_ticker_src_1"
)toml");

  const auto result =
      aquila::tools::bitget::ParseBitgetDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("launch.sources.source_id"), std::string::npos);
  EXPECT_NE(result.error.find("unique"), std::string::npos);
}

TEST(BitgetDataFusionConfigTest, RejectsDuplicateDataShmName) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "bitget_data_fusion_duplicate_shm"
feeds = ["book_ticker"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_0"
data_shm_name = "aquila_bitget_book_ticker_src"

[[launch.sources]]
source_id = 1
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_1"
data_shm_name = "/aquila_bitget_book_ticker_src"
)toml");

  const auto result =
      aquila::tools::bitget::ParseBitgetDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("launch.sources.data_shm_name"),
            std::string::npos);
  EXPECT_NE(result.error.find("unique"), std::string::npos);
}

TEST(BitgetDataFusionConfigTest, RejectsDuplicateMultiFeedChannelName) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "bitget_data_fusion_duplicate_channel"
feeds = ["book_ticker", "trade"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml"
trade = "config/market_data_fusion/bitget_trade_fusion.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_0"
data_shm_name = "aquila_bitget_md_src_0"
book_ticker_channel_name = "same_channel"
trade_channel_name = "same_channel"
)toml");

  const auto result =
      aquila::tools::bitget::ParseBitgetDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("channel_name"), std::string::npos);
}

}  // namespace
