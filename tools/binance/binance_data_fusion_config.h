#ifndef AQUILA_TOOLS_BINANCE_BINANCE_DATA_FUSION_CONFIG_H_
#define AQUILA_TOOLS_BINANCE_BINANCE_DATA_FUSION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "tools/market_data/data_fusion_feed.h"

namespace aquila::tools::binance {

struct BinanceDataFusionSourceConfig {
  std::int32_t source_id{-1};
  std::filesystem::path data_session_config;
  std::string data_session_name;
  std::string book_ticker_shm_name;
  std::string book_ticker_channel_name{"book_ticker_channel"};
  std::string trade_shm_name;
  std::string trade_channel_name{"trade_channel"};
  bool remove_existing_source_shm{true};
  std::int32_t bind_cpu_id{-1};
};

struct BinanceDataFusionConfig {
  std::string name;
  std::filesystem::path fusion_config;
  aquila::tools::market_data::DataFusionFeed feed{
      aquila::tools::market_data::DataFusionFeed::kBookTicker};
  std::vector<BinanceDataFusionSourceConfig> sources;
};

using BinanceDataFusionConfigResult = Result<BinanceDataFusionConfig>;

[[nodiscard]] BinanceDataFusionConfigResult ParseBinanceDataFusionConfig(
    const toml::table& node);

[[nodiscard]] BinanceDataFusionConfigResult LoadBinanceDataFusionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::tools::binance

#endif  // AQUILA_TOOLS_BINANCE_BINANCE_DATA_FUSION_CONFIG_H_
