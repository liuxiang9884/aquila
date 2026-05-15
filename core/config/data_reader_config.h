#ifndef AQUILA_CORE_CONFIG_DATA_READER_CONFIG_H_
#define AQUILA_CORE_CONFIG_DATA_READER_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/common/types.h"
#include "core/config/instrument_catalog.h"

namespace aquila::config {

enum class DataReaderSourceType : std::uint8_t {
  kShm,
  kBinaryFile,
};

enum class DataReaderFeed : std::uint8_t {
  kBookTicker,
};

enum class DataReaderStartPosition : std::uint8_t {
  kLatest,
  kEarliestVisible,
};

enum class DataReaderReadMode : std::uint8_t {
  kLatest,
  kDrain,
};

struct DataReaderSourceConfig {
  std::string name;
  DataReaderSourceType type{DataReaderSourceType::kShm};
  Exchange exchange{Exchange::kGate};
  DataReaderFeed feed{DataReaderFeed::kBookTicker};
  std::string shm_name;
  std::string channel_name;
  std::vector<std::filesystem::path> files;
  DataReaderStartPosition start_position{DataReaderStartPosition::kLatest};
  DataReaderReadMode read_mode{DataReaderReadMode::kLatest};
  bool required{true};
};

struct DataReaderExecutionPolicyConfig {
  std::int32_t bind_cpu_id{-1};
  std::string idle_policy{"spin"};
};

struct DataReaderConfig {
  std::string name;
  std::uint32_t max_events_per_source{64};
  DataReaderExecutionPolicyConfig execution_policy;
  InstrumentCatalog instrument_catalog;
  std::vector<DataReaderSourceConfig> sources;
};

using DataReaderConfigResult = Result<DataReaderConfig>;

[[nodiscard]] DataReaderConfigResult ParseDataReaderConfig(
    const toml::table& node);

[[nodiscard]] DataReaderConfigResult ParseDataReaderConfig(
    const toml::table& node, const std::filesystem::path& config_file_path);

[[nodiscard]] DataReaderConfigResult LoadDataReaderConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_DATA_READER_CONFIG_H_
