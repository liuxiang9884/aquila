#include "tools/bitget/bitget_data_fusion_config.h"

#include <exception>
#include <filesystem>
#include <string>
#include <utility>

#include "tools/market_data/data_fusion_launch_config_parser.h"

namespace aquila::tools::bitget {
namespace {

[[nodiscard]] BitgetDataFusionConfigResult Failure(std::string error) {
  BitgetDataFusionConfigResult result;
  result.error = std::move(error);
  return result;
}

}  // namespace

BitgetDataFusionConfigResult ParseBitgetDataFusionConfig(
    const toml::table& node) {
  return aquila::tools::market_data::ParseDataFusionLaunchConfig<
      BitgetDataFusionConfig, BitgetDataFusionSourceConfig>(node);
}

BitgetDataFusionConfigResult LoadBitgetDataFusionConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseBitgetDataFusionConfig(parsed);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load Bitget data fusion config: "} +
                   exc.what());
  }
}

}  // namespace aquila::tools::bitget
