#ifndef AQUILA_TOOLS_BITGET_BITGET_DATA_FUSION_CONFIG_H_
#define AQUILA_TOOLS_BITGET_BITGET_DATA_FUSION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "tools/market_data/data_fusion_feed.h"

namespace aquila::tools::bitget {

struct BitgetDataFusionSourceConfig {
  std::int32_t source_id{-1};
  std::filesystem::path data_session_config;
  std::string data_session_name;
  std::string data_shm_name;
  std::string book_ticker_channel_name{"book_ticker_channel"};
  std::string trade_channel_name{"trade_channel"};
  bool remove_existing_source_shm{true};
  std::int32_t bind_cpu_id{-1};
};

struct BitgetDataFusionConfig {
  std::string name;
  std::filesystem::path book_ticker_fusion_config;
  std::filesystem::path trade_fusion_config;
  std::vector<aquila::tools::market_data::DataFusionFeed> feeds;
  std::int32_t backend_cpu_affinity{-1};
  std::vector<BitgetDataFusionSourceConfig> sources;
};

using BitgetDataFusionConfigResult = Result<BitgetDataFusionConfig>;

[[nodiscard]] BitgetDataFusionConfigResult ParseBitgetDataFusionConfig(
    const toml::table& node);

[[nodiscard]] BitgetDataFusionConfigResult LoadBitgetDataFusionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::tools::bitget

#endif  // AQUILA_TOOLS_BITGET_BITGET_DATA_FUSION_CONFIG_H_
