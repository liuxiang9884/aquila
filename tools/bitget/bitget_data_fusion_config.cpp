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

[[nodiscard]] bool HasTradeFeed(const BitgetDataFusionConfig& config) noexcept {
  for (aquila::tools::market_data::DataFusionFeed feed : config.feeds) {
    if (feed == aquila::tools::market_data::DataFusionFeed::kTrade) {
      return true;
    }
  }
  return false;
}

}  // namespace

BitgetDataFusionConfigResult ParseBitgetDataFusionConfig(
    const toml::table& node) {
  BitgetDataFusionConfigResult result =
      aquila::tools::market_data::ParseDataFusionLaunchConfig<
          BitgetDataFusionConfig, BitgetDataFusionSourceConfig>(node);
  if (!result.ok) {
    return result;
  }
  if (HasTradeFeed(result.value)) {
    return Failure("Bitget trade feed is unsupported by this adapter");
  }
  return result;
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
