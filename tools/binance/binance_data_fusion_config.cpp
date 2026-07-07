#include "tools/binance/binance_data_fusion_config.h"

#include <exception>
#include <filesystem>
#include <string>
#include <utility>

#include "tools/market_data/data_fusion_launch_config_parser.h"

namespace aquila::tools::binance {
namespace {

[[nodiscard]] BinanceDataFusionConfigResult Failure(std::string error) {
  BinanceDataFusionConfigResult result;
  result.error = std::move(error);
  return result;
}

}  // namespace

BinanceDataFusionConfigResult ParseBinanceDataFusionConfig(
    const toml::table& node) {
  return aquila::tools::market_data::ParseDataFusionLaunchConfig<
      BinanceDataFusionConfig, BinanceDataFusionSourceConfig>(node);
}

BinanceDataFusionConfigResult LoadBinanceDataFusionConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseBinanceDataFusionConfig(parsed);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load Binance data fusion config: "} +
                   exc.what());
  }
}

}  // namespace aquila::tools::binance
