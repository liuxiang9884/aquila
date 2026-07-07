#include "tools/gate/gate_data_fusion_config.h"

#include <exception>
#include <filesystem>
#include <string>
#include <utility>

#include "tools/market_data/data_fusion_launch_config_parser.h"

namespace aquila::tools::gate {
namespace {

[[nodiscard]] GateDataFusionConfigResult Failure(std::string error) {
  GateDataFusionConfigResult result;
  result.error = std::move(error);
  return result;
}

}  // namespace

GateDataFusionConfigResult ParseGateDataFusionConfig(const toml::table& node) {
  return aquila::tools::market_data::ParseDataFusionLaunchConfig<
      GateDataFusionConfig, GateDataFusionSourceConfig>(node);
}

GateDataFusionConfigResult LoadGateDataFusionConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseGateDataFusionConfig(parsed);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load Gate data fusion config: "} +
                   exc.what());
  }
}

}  // namespace aquila::tools::gate
