#include "tools/binance/binance_data_fusion_config.h"

#include <string>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

namespace {

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
