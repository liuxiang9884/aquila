#ifndef AQUILA_TOOLS_GATE_GATE_DATA_FUSION_CONFIG_H_
#define AQUILA_TOOLS_GATE_GATE_DATA_FUSION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"

namespace aquila::tools::gate {

struct GateDataFusionSourceConfig {
  std::int32_t source_id{-1};
  std::filesystem::path data_session_config;
  std::string data_session_name;
  std::string book_ticker_shm_name;
  std::string book_ticker_channel_name{"book_ticker_channel"};
  bool remove_existing_source_shm{true};
  std::int32_t bind_cpu_id{-1};
};

struct GateDataFusionConfig {
  std::string name;
  std::filesystem::path fusion_config;
  std::vector<GateDataFusionSourceConfig> sources;
};

using GateDataFusionConfigResult = Result<GateDataFusionConfig>;

[[nodiscard]] GateDataFusionConfigResult ParseGateDataFusionConfig(
    const toml::table& node);

[[nodiscard]] GateDataFusionConfigResult LoadGateDataFusionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::tools::gate

#endif  // AQUILA_TOOLS_GATE_GATE_DATA_FUSION_CONFIG_H_
