#include "tools/binance/binance_data_fusion_config.h"

#include <string>

#if defined(__linux__)
#include <sched.h>
#endif

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
feeds = ["book_ticker"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session_30symbols_20260604.toml"
data_session_name = "binance_source_0"
data_shm_name = "aquila_binance_book_ticker_src_0"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 16

[[launch.sources]]
source_id = 1
data_session_config = "config/data_sessions/binance_data_session_30symbols_20260604.toml"
data_session_name = "binance_source_1"
data_shm_name = "aquila_binance_book_ticker_src_1"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 17
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "binance_data_fusion_book_ticker_4sources");
  EXPECT_EQ(result.value.backend_cpu_affinity, -1);
  ASSERT_EQ(result.value.feeds.size(), 1U);
  EXPECT_EQ(result.value.feeds[0], support::DataFusionFeed::kBookTicker);
  EXPECT_EQ(
      result.value.book_ticker_fusion_config,
      "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml");
  EXPECT_TRUE(result.value.trade_fusion_config.empty());
  ASSERT_EQ(result.value.sources.size(), 2U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].data_session_name, "binance_source_0");
  EXPECT_EQ(result.value.sources[0].data_shm_name,
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
feeds = ["trade"]

[launch.fusion_configs]
trade = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session_30symbols_20260604.toml"
data_session_name = "binance_trade_source_0"
data_shm_name = "aquila_binance_trade_src_0"
trade_channel_name = "trade_channel"
remove_existing_source_shm = true
bind_cpu_id = 17
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "binance_data_fusion_trade_4sources");
  ASSERT_EQ(result.value.feeds.size(), 1U);
  EXPECT_EQ(result.value.feeds[0], support::DataFusionFeed::kTrade);
  EXPECT_TRUE(result.value.book_ticker_fusion_config.empty());
  EXPECT_EQ(result.value.trade_fusion_config,
            "config/market_data_fusion/binance_trade_fusion_4sources.toml");
  ASSERT_EQ(result.value.sources.size(), 1U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].data_session_name,
            "binance_trade_source_0");
  EXPECT_EQ(result.value.sources[0].data_shm_name,
            "aquila_binance_trade_src_0");
  EXPECT_EQ(result.value.sources[0].trade_channel_name, "trade_channel");
  EXPECT_TRUE(result.value.sources[0].remove_existing_source_shm);
  EXPECT_EQ(result.value.sources[0].bind_cpu_id, 17);
}

TEST(BinanceDataFusionConfigTest, ParsesMultiFeedSources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_book_ticker_trade_4sources"
feeds = ["book_ticker", "trade"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml"
trade = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session_30symbols_20260604.toml"
data_session_name = "binance_source_0"
data_shm_name = "aquila_binance_md_src_0"
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
remove_existing_source_shm = true
bind_cpu_id = 17
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name,
            "binance_data_fusion_book_ticker_trade_4sources");
  EXPECT_EQ(result.value.backend_cpu_affinity, -1);
  ASSERT_EQ(result.value.feeds.size(), 2U);
  EXPECT_EQ(result.value.feeds[0], support::DataFusionFeed::kBookTicker);
  EXPECT_EQ(result.value.feeds[1], support::DataFusionFeed::kTrade);
  EXPECT_EQ(
      result.value.book_ticker_fusion_config,
      "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml");
  EXPECT_EQ(result.value.trade_fusion_config,
            "config/market_data_fusion/binance_trade_fusion_4sources.toml");
  ASSERT_EQ(result.value.sources.size(), 1U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].data_shm_name, "aquila_binance_md_src_0");
  EXPECT_EQ(result.value.sources[0].book_ticker_channel_name,
            "book_ticker_channel");
  EXPECT_EQ(result.value.sources[0].trade_channel_name, "trade_channel");
  EXPECT_TRUE(result.value.sources[0].remove_existing_source_shm);
  EXPECT_EQ(result.value.sources[0].bind_cpu_id, 17);
}

TEST(BinanceDataFusionConfigTest, RejectsUnknownFeed) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_unknown"
feeds = ["depth"]

[launch.fusion_configs]
trade = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_0"
data_shm_name = "aquila_binance_trade_src_0"
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("launch.feeds"), std::string::npos);
}

TEST(BinanceDataFusionConfigTest, RejectsMissingFeeds) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_missing_feeds"

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_0"
data_shm_name = "aquila_binance_md_src_0"
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("launch.feeds"), std::string::npos);
}

TEST(BinanceDataFusionConfigTest, RejectsDuplicateMultiFeed) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_duplicate_feed"
feeds = ["trade", "trade"]

[launch.fusion_configs]
trade = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_0"
data_shm_name = "aquila_binance_md_src_0"
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("duplicate feed trade"), std::string::npos);
}

TEST(BinanceDataFusionConfigTest, RejectsMissingEnabledFeedFusionConfig) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_missing_book_ticker_config"
feeds = ["book_ticker", "trade"]

[launch.fusion_configs]
trade = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_0"
data_shm_name = "aquila_binance_md_src_0"
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("launch.fusion_configs.book_ticker"),
            std::string::npos);
}

TEST(BinanceDataFusionConfigTest, RejectsMissingDataShmName) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_missing_data_shm"
feeds = ["trade"]

[launch.fusion_configs]
trade = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_0"
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("launch.sources.data_shm_name"),
            std::string::npos);
}

TEST(BinanceDataFusionConfigTest, RejectsDuplicateDataShmName) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_duplicate_data_shm"
feeds = ["trade"]

[launch.fusion_configs]
trade = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_0"
data_shm_name = "aquila_binance_trade_src"

[[launch.sources]]
source_id = 1
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_1"
data_shm_name = "/aquila_binance_trade_src"
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("launch.sources.data_shm_name"),
            std::string::npos);
  EXPECT_NE(result.error.find("unique"), std::string::npos);
}

TEST(BinanceDataFusionConfigTest, RejectsDuplicateMultiFeedChannelName) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_duplicate_channel"
feeds = ["book_ticker", "trade"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml"
trade = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_0"
data_shm_name = "aquila_binance_md_src_0"
book_ticker_channel_name = "same_channel"
trade_channel_name = "same_channel"
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("channel_name"), std::string::npos);
}

TEST(BinanceDataFusionConfigTest, RejectsInvalidSourceCpuBinding) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_bad_cpu"
feeds = ["trade"]

[launch.fusion_configs]
trade = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_0"
data_shm_name = "aquila_binance_trade_src_0"
bind_cpu_id = -2
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("launch.sources.bind_cpu_id"), std::string::npos);
}

TEST(BinanceDataFusionConfigTest, RejectsLogBackendCpuAtCpuSetSize) {
#if !defined(__linux__)
  GTEST_SKIP() << "CPU_SETSIZE is Linux-specific";
#else
  const std::string text = std::string{R"toml(
[log]
backend_cpu_affinity = )toml"} + std::to_string(CPU_SETSIZE) +
                           R"toml(

[launch]
name = "binance_data_fusion_bad_log_cpu"
feeds = ["trade"]

[launch.fusion_configs]
trade = "config/market_data_fusion/binance_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_0"
data_shm_name = "aquila_binance_trade_src_0"
)toml";
  const toml::parse_result parsed = ParseToml(text);

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("log.backend_cpu_affinity"), std::string::npos);
#endif
}

TEST(BinanceDataFusionConfigTest, RejectsDuplicateSourceId) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "binance_data_fusion_book_ticker"
feeds = ["book_ticker"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 1
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_1a"
data_shm_name = "aquila_binance_book_ticker_src_1a"

[[launch.sources]]
source_id = 1
data_session_config = "config/data_sessions/binance_data_session.toml"
data_session_name = "binance_source_1b"
data_shm_name = "aquila_binance_book_ticker_src_1b"
)toml");

  const auto result =
      aquila::tools::binance::ParseBinanceDataFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("source_id"), std::string::npos);
}

}  // namespace
