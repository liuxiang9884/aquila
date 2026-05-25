#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "tools/market_data/data_reader_recorder.h"

namespace aquila::tools::market_data {

struct RecorderConfig {
  RecorderRotationConfig rotation;
};

using RecorderConfigResult = Result<RecorderConfig>;

[[nodiscard]] RecorderConfigResult ParseRecorderConfig(
    const toml::table& node, const std::filesystem::path& output_path,
    RecorderWriteMode write_mode);

}  // namespace aquila::tools::market_data
