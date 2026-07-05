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
  RecorderRotationConfig trade_rotation;
};

using RecorderConfigResult = Result<RecorderConfig>;

[[nodiscard]] RecorderConfigResult ParseRecorderConfig(
    const toml::table& node, const std::filesystem::path& output_path,
    RecorderWriteMode write_mode);

[[nodiscard]] std::filesystem::path DeriveTradeOutputPath(
    const std::filesystem::path& book_ticker_output_path);

}  // namespace aquila::tools::market_data
