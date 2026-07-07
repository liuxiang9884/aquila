#include "core/config/book_ticker_fusion_config.h"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/common/fusion_metadata_mode.h"

namespace {

toml::parse_result ParseToml(const std::string& text) {
  return toml::parse(text);
}

TEST(BookTickerFusionConfigTest, ParsesFourSources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_bbo_fusion_4src"
max_events_per_source = 1
bind_cpu_id = 16
max_symbol_id = 4096

[fusion.output]
shm_name = "aquila_gate_book_ticker_fusion"
channel_name = "book_ticker_channel"
remove_existing = true
metadata_bin = "/home/liuxiang/tmp/gate_fusion/fusion_metadata.bin"

[[fusion.sources]]
source_id = 0
name = "gate_src_0"
shm_name = "aquila_gate_book_ticker_src_0"
channel_name = "book_ticker_channel"

[[fusion.sources]]
source_id = 1
name = "gate_src_1"
shm_name = "aquila_gate_book_ticker_src_1"
channel_name = "book_ticker_channel"

[[fusion.sources]]
source_id = 2
name = "gate_src_2"
shm_name = "aquila_gate_book_ticker_src_2"
channel_name = "book_ticker_channel"

[[fusion.sources]]
source_id = 3
name = "gate_src_3"
shm_name = "aquila_gate_book_ticker_src_3"
channel_name = "book_ticker_channel"
)toml");

  const auto result = aquila::config::ParseBookTickerFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "gate_bbo_fusion_4src");
  EXPECT_EQ(result.value.max_events_per_source, 1U);
  EXPECT_EQ(result.value.bind_cpu_id, 16);
  EXPECT_EQ(result.value.max_symbol_id, 4096U);
  EXPECT_EQ(result.value.output.shm_name, "aquila_gate_book_ticker_fusion");
  EXPECT_EQ(result.value.output.channel_name, "book_ticker_channel");
  EXPECT_TRUE(result.value.output.remove_existing);
  EXPECT_EQ(result.value.output.metadata_bin,
            "/home/liuxiang/tmp/gate_fusion/fusion_metadata.bin");
  ASSERT_EQ(result.value.sources.size(), 4U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].name, "gate_src_0");
  EXPECT_EQ(result.value.sources[0].shm_name, "aquila_gate_book_ticker_src_0");
  EXPECT_EQ(result.value.sources[3].source_id, 3);
  EXPECT_EQ(result.value.sources[3].name, "gate_src_3");
  EXPECT_EQ(result.value.sources[3].shm_name, "aquila_gate_book_ticker_src_3");
}

TEST(BookTickerFusionConfigTest, LoadsCommittedBinanceFourSourceConfig) {
  const std::filesystem::path config_path =
      std::filesystem::path{AQUILA_PROJECT_SOURCE_DIR} /
      "config/market_data_fusion/binance_book_ticker_fusion_4sources.toml";

  const auto result =
      aquila::config::LoadBookTickerFusionConfigFile(config_path);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "binance_book_ticker_fusion_4sources");
  EXPECT_EQ(result.value.max_events_per_source, 1U);
  EXPECT_EQ(result.value.bind_cpu_id, 16);
  EXPECT_EQ(result.value.output.shm_name, "aquila_binance_book_ticker_fusion");
  EXPECT_EQ(result.value.output.channel_name, "book_ticker_channel");
  EXPECT_TRUE(result.value.output.remove_existing);
  ASSERT_EQ(result.value.sources.size(), 4U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].name, "binance_source_0");
  EXPECT_EQ(result.value.sources[0].shm_name,
            "aquila_binance_book_ticker_src_0");
  EXPECT_EQ(result.value.sources[3].source_id, 3);
  EXPECT_EQ(result.value.sources[3].name, "binance_source_3");
  EXPECT_EQ(result.value.sources[3].shm_name,
            "aquila_binance_book_ticker_src_3");
}

TEST(BookTickerFusionConfigTest, RejectsDuplicateSourceId) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_bbo_fusion"

[fusion.output]
shm_name = "aquila_gate_book_ticker_fusion"
channel_name = "book_ticker_channel"
metadata_bin = "/home/liuxiang/tmp/gate_fusion/fusion_metadata.bin"

[[fusion.sources]]
source_id = 1
name = "gate_src_1a"
shm_name = "aquila_gate_book_ticker_src_1a"
channel_name = "book_ticker_channel"

[[fusion.sources]]
source_id = 1
name = "gate_src_1b"
shm_name = "aquila_gate_book_ticker_src_1b"
channel_name = "book_ticker_channel"
)toml");

  const auto result = aquila::config::ParseBookTickerFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("source_id"), std::string::npos);
}

TEST(BookTickerFusionConfigTest, RejectsEmptySources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_bbo_fusion"

[fusion.output]
shm_name = "aquila_gate_book_ticker_fusion"
channel_name = "book_ticker_channel"
metadata_bin = "/home/liuxiang/tmp/gate_fusion/fusion_metadata.bin"
)toml");

  const auto result = aquila::config::ParseBookTickerFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("sources"), std::string::npos);
}

TEST(BookTickerFusionConfigTest, HandlesMissingMetadataBinForBuildMode) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_bbo_fusion"

[fusion.output]
shm_name = "aquila_gate_book_ticker_fusion"
channel_name = "book_ticker_channel"

[[fusion.sources]]
source_id = 0
name = "gate_src_0"
shm_name = "aquila_gate_book_ticker_src_0"
channel_name = "book_ticker_channel"
)toml");

  const auto result = aquila::config::ParseBookTickerFusionConfig(parsed);

#if AQUILA_FUSION_METADATA_ENABLED
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("metadata_bin"), std::string::npos);
#else
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.output.metadata_bin.empty());
#endif
}

}  // namespace
