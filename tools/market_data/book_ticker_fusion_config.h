#ifndef AQUILA_TOOLS_MARKET_DATA_BOOK_TICKER_FUSION_CONFIG_H_
#define AQUILA_TOOLS_MARKET_DATA_BOOK_TICKER_FUSION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"

namespace aquila::tools::market_data {

struct BookTickerFusionSourceConfig {
  std::int32_t source_id{-1};
  std::string name;
  std::string shm_name;
  std::string channel_name;
};

struct BookTickerFusionOutputConfig {
  std::string shm_name;
  std::string channel_name;
  bool remove_existing{false};
  std::filesystem::path metadata_bin;
};

struct BookTickerFusionConfig {
  std::string name;
  std::uint32_t max_events_per_source{1};
  std::int32_t bind_cpu_id{-1};
  std::uint32_t max_symbol_id{4096};
  BookTickerFusionOutputConfig output;
  std::vector<BookTickerFusionSourceConfig> sources;
};

using BookTickerFusionConfigResult = Result<BookTickerFusionConfig>;

[[nodiscard]] BookTickerFusionConfigResult ParseBookTickerFusionConfig(
    const toml::table& node);

[[nodiscard]] BookTickerFusionConfigResult LoadBookTickerFusionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::tools::market_data

#endif  // AQUILA_TOOLS_MARKET_DATA_BOOK_TICKER_FUSION_CONFIG_H_
