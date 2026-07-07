#include "core/config/fusion_config_parser.h"

#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/market_data/fusion/config.h"

namespace {

namespace md = aquila::market_data;

struct ParserTraits {
  using Config = md::BookTickerFusionConfig;
  using SourceConfig = md::BookTickerFusionSourceConfig;
  using Result = aquila::Result<Config>;

  static constexpr std::string_view kDefaultSourceChannel =
      "unit_default_channel";
  static constexpr std::string_view kLoadErrorPrefix = "unit load: ";
};

toml::parse_result ParseToml(const std::string& text) {
  return toml::parse(text);
}

TEST(FusionConfigParserTest, AppliesTraitDefaultSourceChannel) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "unit_fusion"

[fusion.output]
shm_name = "unit_output"
channel_name = "unit_output_channel"
metadata_bin = "/home/liuxiang/tmp/unit_fusion_metadata.bin"

[[fusion.sources]]
source_id = 0
name = "unit_source"
shm_name = "unit_source_shm"
)toml");

  const ParserTraits::Result result =
      aquila::config::ParseFusionConfig<ParserTraits>(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.sources.size(), 1U);
  EXPECT_EQ(result.value.sources[0].channel_name, "unit_default_channel");
}

TEST(FusionConfigParserTest, RejectsDuplicateSourceIds) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "unit_fusion"

[fusion.output]
shm_name = "unit_output"
channel_name = "unit_output_channel"
metadata_bin = "/home/liuxiang/tmp/unit_fusion_metadata.bin"

[[fusion.sources]]
source_id = 7
name = "unit_source_a"
shm_name = "unit_source_shm_a"

[[fusion.sources]]
source_id = 7
name = "unit_source_b"
shm_name = "unit_source_shm_b"
)toml");

  const ParserTraits::Result result =
      aquila::config::ParseFusionConfig<ParserTraits>(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("source_id"), std::string::npos);
}

TEST(FusionConfigParserTest, RejectsInvalidBindCpuId) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "unit_fusion"
bind_cpu_id = -2

[fusion.output]
shm_name = "unit_output"
channel_name = "unit_output_channel"
metadata_bin = "/home/liuxiang/tmp/unit_fusion_metadata.bin"

[[fusion.sources]]
source_id = 0
name = "unit_source"
shm_name = "unit_source_shm"
)toml");

  const ParserTraits::Result result =
      aquila::config::ParseFusionConfig<ParserTraits>(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("fusion.bind_cpu_id"), std::string::npos);
}

TEST(FusionConfigParserTest, RejectsOutOfRangeBindCpuId) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "unit_fusion"
bind_cpu_id = 2147483648

[fusion.output]
shm_name = "unit_output"
channel_name = "unit_output_channel"
metadata_bin = "/home/liuxiang/tmp/unit_fusion_metadata.bin"

[[fusion.sources]]
source_id = 0
name = "unit_source"
shm_name = "unit_source_shm"
)toml");

  const ParserTraits::Result result =
      aquila::config::ParseFusionConfig<ParserTraits>(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("fusion.bind_cpu_id"), std::string::npos);
}

TEST(FusionConfigParserTest, RejectsOutOfRangeSourceId) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "unit_fusion"

[fusion.output]
shm_name = "unit_output"
channel_name = "unit_output_channel"
metadata_bin = "/home/liuxiang/tmp/unit_fusion_metadata.bin"

[[fusion.sources]]
source_id = 2147483648
name = "unit_source"
shm_name = "unit_source_shm"
)toml");

  const ParserTraits::Result result =
      aquila::config::ParseFusionConfig<ParserTraits>(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("fusion.sources.source_id"), std::string::npos);
}

}  // namespace
