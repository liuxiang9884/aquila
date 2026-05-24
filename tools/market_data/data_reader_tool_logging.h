#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include "core/config/data_reader_config.h"

namespace aquila::tools::market_data {

[[nodiscard]] inline std::string_view ReadModeName(
    config::DataReaderReadMode read_mode) noexcept {
  switch (read_mode) {
    case config::DataReaderReadMode::kLatest:
      return "latest";
    case config::DataReaderReadMode::kDrain:
      return "drain";
  }
  return "unknown";
}

[[nodiscard]] inline std::string_view StartPositionName(
    config::DataReaderStartPosition start_position) noexcept {
  switch (start_position) {
    case config::DataReaderStartPosition::kLatest:
      return "latest";
    case config::DataReaderStartPosition::kEarliestVisible:
      return "earliest_visible";
  }
  return "unknown";
}

[[nodiscard]] inline std::string JoinFiles(
    const std::vector<std::filesystem::path>& files) {
  std::string joined;
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (i != 0) {
      joined.append(",");
    }
    joined.append(files[i].string());
  }
  return joined;
}

[[nodiscard]] inline std::string FormatSourceConfigLog(
    std::size_t index, const config::DataReaderSourceConfig& source) {
  return fmt::format(
      "source_config index={} name={} exchange={} type={} feed={} "
      "start_position={} read_mode={} shm_name={} channel_name={} files=[{}]",
      index, source.name, magic_enum::enum_name(source.exchange),
      magic_enum::enum_name(source.type), magic_enum::enum_name(source.feed),
      StartPositionName(source.start_position), ReadModeName(source.read_mode),
      source.shm_name.empty() ? "none" : source.shm_name,
      source.channel_name.empty() ? "none" : source.channel_name,
      JoinFiles(source.files));
}

[[nodiscard]] inline std::string FormatToolStartupLog(
    std::string_view tool, std::string_view mode,
    const std::filesystem::path& config_path,
    std::optional<std::filesystem::path> output_path, std::uint64_t max_polls,
    std::uint64_t drain_budget, std::size_t book_ticker_abi_size) {
  return fmt::format(
      "tool={} mode={} config={} output={} max_polls={} "
      "max_events_per_drain={} book_ticker_abi_size={}",
      tool, mode, config_path.string(),
      output_path.has_value() ? output_path->string() : std::string{"none"},
      max_polls, drain_budget, book_ticker_abi_size);
}

}  // namespace aquila::tools::market_data
