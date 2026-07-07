#ifndef AQUILA_CORE_CONFIG_TRADE_FUSION_CONFIG_H_
#define AQUILA_CORE_CONFIG_TRADE_FUSION_CONFIG_H_

#include <filesystem>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/market_data/fusion/config.h"

namespace aquila::config {

using TradeFusionConfigResult = Result<aquila::market_data::TradeFusionConfig>;

[[nodiscard]] TradeFusionConfigResult ParseTradeFusionConfig(
    const toml::table& node);

[[nodiscard]] TradeFusionConfigResult LoadTradeFusionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_TRADE_FUSION_CONFIG_H_
