#include "core/config/trade_fusion_config.h"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/common/fusion_metadata_mode.h"

namespace {

toml::parse_result ParseToml(const std::string& text) {
  return toml::parse(text);
}

TEST(TradeFusionConfigTest, ParsesFourSources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_trade_fusion_4src"
max_events_per_source = 1
bind_cpu_id = 16
max_symbol_id = 4096

[fusion.output]
shm_name = "aquila_gate_trade_fusion"
channel_name = "trade_channel"
remove_existing = true
metadata_bin = "/home/liuxiang/tmp/gate_trade_fusion/fusion_metadata.bin"

[[fusion.sources]]
source_id = 0
name = "gate_trade_src_0"
shm_name = "aquila_gate_trade_src_0"
channel_name = "trade_channel"

[[fusion.sources]]
source_id = 1
name = "gate_trade_src_1"
shm_name = "aquila_gate_trade_src_1"
channel_name = "trade_channel"
)toml");

  const auto result = aquila::config::ParseTradeFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "gate_trade_fusion_4src");
  EXPECT_EQ(result.value.max_events_per_source, 1U);
  EXPECT_EQ(result.value.bind_cpu_id, 16);
  EXPECT_EQ(result.value.max_symbol_id, 4096U);
  EXPECT_EQ(result.value.output.shm_name, "aquila_gate_trade_fusion");
  EXPECT_EQ(result.value.output.channel_name, "trade_channel");
  EXPECT_TRUE(result.value.output.remove_existing);
  EXPECT_EQ(result.value.output.metadata_bin,
            "/home/liuxiang/tmp/gate_trade_fusion/fusion_metadata.bin");
  ASSERT_EQ(result.value.sources.size(), 2U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].name, "gate_trade_src_0");
  EXPECT_EQ(result.value.sources[0].shm_name, "aquila_gate_trade_src_0");
}

TEST(TradeFusionConfigTest, LoadsCommittedBinanceFourSourceConfig) {
  const std::filesystem::path config_path =
      std::filesystem::path{AQUILA_PROJECT_SOURCE_DIR} /
      "config/market_data_fusion/binance_trade_fusion_4sources.toml";

  const auto result = aquila::config::LoadTradeFusionConfigFile(config_path);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "binance_trade_fusion_4sources");
  EXPECT_EQ(result.value.output.shm_name, "aquila_binance_trade_fusion");
  EXPECT_EQ(result.value.output.channel_name, "trade_channel");
  ASSERT_EQ(result.value.sources.size(), 4U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].name, "binance_trade_source_0");
  EXPECT_EQ(result.value.sources[0].shm_name, "aquila_binance_trade_src_0");
}

TEST(TradeFusionConfigTest, RejectsDuplicateSourceId) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_trade_fusion"

[fusion.output]
shm_name = "aquila_gate_trade_fusion"
channel_name = "trade_channel"
metadata_bin = "/home/liuxiang/tmp/gate_trade_fusion/fusion_metadata.bin"

[[fusion.sources]]
source_id = 1
name = "gate_trade_src_1a"
shm_name = "aquila_gate_trade_src_1a"

[[fusion.sources]]
source_id = 1
name = "gate_trade_src_1b"
shm_name = "aquila_gate_trade_src_1b"
)toml");

  const auto result = aquila::config::ParseTradeFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("source_id"), std::string::npos);
}

TEST(TradeFusionConfigTest, HandlesMissingMetadataBinForBuildMode) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_trade_fusion"

[fusion.output]
shm_name = "aquila_gate_trade_fusion"
channel_name = "trade_channel"

[[fusion.sources]]
source_id = 0
name = "gate_trade_src_0"
shm_name = "aquila_gate_trade_src_0"
)toml");

  const auto result = aquila::config::ParseTradeFusionConfig(parsed);

#if AQUILA_FUSION_METADATA_ENABLED
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("metadata_bin"), std::string::npos);
#else
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.output.metadata_bin.empty());
#endif
}

}  // namespace
