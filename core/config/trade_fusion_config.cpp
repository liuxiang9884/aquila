#include "core/config/trade_fusion_config.h"

#include <filesystem>
#include <string_view>

#include "core/config/fusion_config_parser.h"

namespace aquila::config {
namespace {

namespace md = aquila::market_data;

struct TradeFusionConfigParseTraits {
  using Config = md::TradeFusionConfig;
  using SourceConfig = md::TradeFusionSourceConfig;
  using Result = TradeFusionConfigResult;

  static constexpr std::string_view kDefaultSourceChannel = "trade_channel";
  static constexpr std::string_view kLoadErrorPrefix =
      "failed to load trade fusion config: ";
};

}  // namespace

TradeFusionConfigResult ParseTradeFusionConfig(const toml::table& node) {
  return ParseFusionConfig<TradeFusionConfigParseTraits>(node);
}

TradeFusionConfigResult LoadTradeFusionConfigFile(
    const std::filesystem::path& path) {
  return LoadFusionConfigFile<TradeFusionConfigParseTraits>(path);
}

}  // namespace aquila::config
