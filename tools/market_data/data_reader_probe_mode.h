#pragma once

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string_view>

#include "core/config/data_reader_config.h"

namespace aquila::tools::market_data {

enum class DataReaderProbeMode : std::uint8_t {
  kRealtime,
  kHistorical,
};

[[nodiscard]] inline DataReaderProbeMode DetectProbeMode(
    const config::DataReaderConfig& config) {
  if (config.sources.empty()) {
    throw std::invalid_argument("data reader probe requires at least one source");
  }

  std::size_t shm_sources = 0;
  std::size_t binary_sources = 0;
  for (const config::DataReaderSourceConfig& source : config.sources) {
    switch (source.type) {
      case config::DataReaderSourceType::kShm:
        ++shm_sources;
        break;
      case config::DataReaderSourceType::kBinaryFile:
        ++binary_sources;
        break;
    }
  }

  if (shm_sources == config.sources.size()) {
    return DataReaderProbeMode::kRealtime;
  }
  if (binary_sources == 1 && config.sources.size() == 1) {
    return DataReaderProbeMode::kHistorical;
  }
  throw std::invalid_argument(
      "data reader probe requires all shm sources or exactly one binary_file "
      "source");
}

[[nodiscard]] inline std::string_view ProbeModeName(
    DataReaderProbeMode mode) noexcept {
  switch (mode) {
    case DataReaderProbeMode::kRealtime:
      return "realtime";
    case DataReaderProbeMode::kHistorical:
      return "historical";
  }
  return "unknown";
}

}  // namespace aquila::tools::market_data
