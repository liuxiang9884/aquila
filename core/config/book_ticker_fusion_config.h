#ifndef AQUILA_CORE_CONFIG_BOOK_TICKER_FUSION_CONFIG_H_
#define AQUILA_CORE_CONFIG_BOOK_TICKER_FUSION_CONFIG_H_

#include <filesystem>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/market_data/fusion/config.h"

namespace aquila::config {

using BookTickerFusionConfigResult =
    Result<aquila::market_data::BookTickerFusionConfig>;

[[nodiscard]] BookTickerFusionConfigResult ParseBookTickerFusionConfig(
    const toml::table& node);

[[nodiscard]] BookTickerFusionConfigResult LoadBookTickerFusionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_BOOK_TICKER_FUSION_CONFIG_H_
