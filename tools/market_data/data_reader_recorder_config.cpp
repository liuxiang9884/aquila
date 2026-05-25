#include "tools/market_data/data_reader_recorder_config.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace aquila::tools::market_data {
namespace {

[[nodiscard]] RecorderConfigResult Failure(std::string error) {
  RecorderConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] RecorderConfigResult Success(RecorderConfig config) {
  RecorderConfigResult result;
  result.ok = true;
  result.value = std::move(config);
  return result;
}

[[nodiscard]] std::string DefaultFilePrefix(
    const std::filesystem::path& output_path) {
  const std::string stem = output_path.stem().string();
  return stem.empty() ? std::string{"book_ticker"} : stem;
}

[[nodiscard]] std::filesystem::path DefaultOutputDir(
    const std::filesystem::path& output_path) {
  return output_path.parent_path() / "segments";
}

[[nodiscard]] std::filesystem::path DefaultManifestPath(
    const std::filesystem::path& output_path, std::string_view file_prefix) {
  return output_path.parent_path() /
         fmt::format("{}_manifest.jsonl", file_prefix);
}

[[nodiscard]] bool BoolOr(toml::node_view<const toml::node> node, bool fallback,
                          std::string_view field_name, std::string* error) {
  if (!node) {
    return fallback;
  }
  const std::optional<bool> value = node.value<bool>();
  if (!value.has_value()) {
    *error = fmt::format("recorder.{} must be a boolean", field_name);
    return fallback;
  }
  return *value;
}

[[nodiscard]] std::uint32_t UInt32Or(toml::node_view<const toml::node> node,
                                     std::uint32_t fallback,
                                     std::string_view field_name,
                                     std::string* error) {
  if (!node) {
    return fallback;
  }
  const std::optional<std::int64_t> value = node.value<std::int64_t>();
  if (!value.has_value()) {
    *error = fmt::format("recorder.{} must be an integer", field_name);
    return fallback;
  }
  if (*value <= 0) {
    *error = fmt::format("recorder.{} must be positive", field_name);
    return fallback;
  }
  if (*value >
      static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
    *error = fmt::format("recorder.{} exceeds uint32 max", field_name);
    return fallback;
  }
  return static_cast<std::uint32_t>(*value);
}

[[nodiscard]] std::string StringOr(toml::node_view<const toml::node> node,
                                   std::string fallback,
                                   std::string_view field_name,
                                   std::string* error) {
  if (!node) {
    return fallback;
  }
  const std::optional<std::string> value = node.value<std::string>();
  if (!value.has_value()) {
    *error = fmt::format("recorder.{} must be a string", field_name);
    return fallback;
  }
  return *value;
}

}  // namespace

RecorderConfigResult ParseRecorderConfig(
    const toml::table& node, const std::filesystem::path& output_path,
    RecorderWriteMode write_mode) {
  RecorderConfig config;
  config.rotation.output_dir = DefaultOutputDir(output_path);
  config.rotation.file_prefix = DefaultFilePrefix(output_path);
  config.rotation.manifest_path =
      DefaultManifestPath(output_path, config.rotation.file_prefix);

  const toml::table* recorder = node["recorder"].as_table();
  if (recorder == nullptr) {
    return Success(std::move(config));
  }

  std::string error;
  config.rotation.enabled =
      BoolOr((*recorder)["rotation_enabled"], config.rotation.enabled,
             "rotation_enabled", &error);
  if (!error.empty()) {
    return Failure(std::move(error));
  }

  config.rotation.rotation_interval_sec = UInt32Or(
      (*recorder)["rotation_interval_sec"],
      config.rotation.rotation_interval_sec, "rotation_interval_sec", &error);
  if (!error.empty()) {
    return Failure(std::move(error));
  }

  config.rotation.output_dir = std::filesystem::path{
      StringOr((*recorder)["output_dir"], config.rotation.output_dir.string(),
               "output_dir", &error)};
  if (!error.empty()) {
    return Failure(std::move(error));
  }
  config.rotation.file_prefix =
      StringOr((*recorder)["file_prefix"], config.rotation.file_prefix,
               "file_prefix", &error);
  if (!error.empty()) {
    return Failure(std::move(error));
  }
  config.rotation.manifest_path = std::filesystem::path{StringOr(
      (*recorder)["manifest_path"], config.rotation.manifest_path.string(),
      "manifest_path", &error)};
  if (!error.empty()) {
    return Failure(std::move(error));
  }

  if (config.rotation.output_dir.empty()) {
    return Failure("recorder.output_dir must not be empty");
  }
  if (config.rotation.file_prefix.empty()) {
    return Failure("recorder.file_prefix must not be empty");
  }
  if (config.rotation.manifest_path.empty()) {
    return Failure("recorder.manifest_path must not be empty");
  }
  if (config.rotation.enabled && write_mode == RecorderWriteMode::kAppend) {
    return Failure("recorder rotation does not support append mode");
  }

  return Success(std::move(config));
}

}  // namespace aquila::tools::market_data
