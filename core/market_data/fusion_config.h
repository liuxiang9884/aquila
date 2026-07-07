#ifndef AQUILA_CORE_MARKET_DATA_FUSION_CONFIG_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aquila::market_data {

struct FusionSourceConfig {
  std::int32_t source_id{-1};
  std::string name;
  std::string shm_name;
  std::string channel_name;
};

struct FusionOutputConfig {
  std::string shm_name;
  std::string channel_name;
  bool remove_existing{false};
  std::filesystem::path metadata_bin;
};

template <typename SourceConfig, typename OutputConfig>
struct BasicFusionConfig {
  std::string name;
  std::uint32_t max_events_per_source{1};
  std::int32_t bind_cpu_id{-1};
  std::uint32_t max_symbol_id{4096};
  OutputConfig output;
  std::vector<SourceConfig> sources;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_CONFIG_H_
