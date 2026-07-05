#include "tools/market_data/data_reader_recorder_config.h"

#include <cstdint>
#include <exception>
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

[[nodiscard]] RecorderValidationResult ValidationFailure(std::string error) {
  RecorderValidationResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] RecorderValidationResult ValidationSuccess() {
  RecorderValidationResult result;
  result.ok = true;
  result.value = true;
  return result;
}

[[nodiscard]] std::optional<std::string> TerminalSymlinkError(
    const std::filesystem::path& path, std::string_view field_name) {
  try {
    if (RecorderPathIsTerminalSymlink(path, field_name)) {
      return fmt::format("{} must not be a symlink", field_name);
    }
  } catch (const std::exception& exc) {
    return exc.what();
  }
  return std::nullopt;
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

[[nodiscard]] std::string DeriveTradeName(std::string name) {
  constexpr std::string_view kBookTickerToken{"book_ticker"};
  constexpr std::string_view kTradeToken{"trade"};
  const std::size_t position = name.find(kBookTickerToken);
  if (position != std::string::npos) {
    name.replace(position, kBookTickerToken.size(), kTradeToken);
    return name;
  }
  name.append("_trade");
  return name;
}

[[nodiscard]] std::string DeriveTradeManifestStem(
    std::string manifest_stem, std::string_view book_ticker_file_prefix,
    std::string_view trade_file_prefix) {
  const std::size_t prefix_position =
      manifest_stem.find(book_ticker_file_prefix);
  if (!book_ticker_file_prefix.empty() &&
      prefix_position != std::string::npos) {
    manifest_stem.replace(prefix_position, book_ticker_file_prefix.size(),
                          trade_file_prefix);
    return manifest_stem;
  }
  return DeriveTradeName(std::move(manifest_stem));
}

[[nodiscard]] RecorderRotationConfig DeriveTradeRotationConfig(
    const RecorderRotationConfig& book_ticker_config) {
  RecorderRotationConfig trade_config = book_ticker_config;
  trade_config.output_dir =
      book_ticker_config.output_dir.parent_path() /
      DeriveTradeName(book_ticker_config.output_dir.filename().string());
  trade_config.file_prefix = DeriveTradeName(book_ticker_config.file_prefix);
  trade_config.manifest_path =
      book_ticker_config.manifest_path.parent_path() /
      (DeriveTradeManifestStem(book_ticker_config.manifest_path.stem().string(),
                               book_ticker_config.file_prefix,
                               trade_config.file_prefix) +
       book_ticker_config.manifest_path.extension().string());
  return trade_config;
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
  config.trade_rotation = DeriveTradeRotationConfig(config.rotation);

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

  config.trade_rotation = DeriveTradeRotationConfig(config.rotation);
  config.trade_rotation.output_dir = std::filesystem::path{StringOr(
      (*recorder)["trade_output_dir"],
      config.trade_rotation.output_dir.string(), "trade_output_dir", &error)};
  if (!error.empty()) {
    return Failure(std::move(error));
  }
  config.trade_rotation.file_prefix =
      StringOr((*recorder)["trade_file_prefix"],
               config.trade_rotation.file_prefix, "trade_file_prefix", &error);
  if (!error.empty()) {
    return Failure(std::move(error));
  }
  config.trade_rotation.manifest_path = std::filesystem::path{
      StringOr((*recorder)["trade_manifest_path"],
               config.trade_rotation.manifest_path.string(),
               "trade_manifest_path", &error)};
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
  if (config.trade_rotation.output_dir.empty()) {
    return Failure("recorder.trade_output_dir must not be empty");
  }
  if (config.trade_rotation.file_prefix.empty()) {
    return Failure("recorder.trade_file_prefix must not be empty");
  }
  if (config.trade_rotation.manifest_path.empty()) {
    return Failure("recorder.trade_manifest_path must not be empty");
  }
  if (config.rotation.enabled && write_mode == RecorderWriteMode::kAppend) {
    return Failure("recorder rotation does not support append mode");
  }
  if (config.rotation.enabled) {
    if (const std::optional<std::string> error = TerminalSymlinkError(
            config.rotation.manifest_path, "recorder.manifest_path");
        error.has_value()) {
      return Failure(*error);
    }
    if (const std::optional<std::string> error =
            TerminalSymlinkError(config.trade_rotation.manifest_path,
                                 "recorder.trade_manifest_path");
        error.has_value()) {
      return Failure(*error);
    }
  }
  if (config.rotation.enabled &&
      RecorderSamePath(config.rotation.manifest_path,
                       config.trade_rotation.manifest_path)) {
    return Failure(
        "recorder.manifest_path and recorder.trade_manifest_path must be "
        "different");
  }
  if (config.rotation.enabled &&
      RecorderSamePath(config.rotation.output_dir / config.rotation.file_prefix,
                       config.trade_rotation.output_dir /
                           config.trade_rotation.file_prefix)) {
    return Failure(
        "recorder.output_dir/file_prefix and "
        "recorder.trade_output_dir/trade_file_prefix must describe different "
        "segment namespaces");
  }

  return Success(std::move(config));
}

std::filesystem::path DeriveTradeOutputPath(
    const std::filesystem::path& book_ticker_output_path) {
  const std::filesystem::path parent = book_ticker_output_path.parent_path();
  const std::string stem = book_ticker_output_path.stem().string();
  const std::string extension = book_ticker_output_path.extension().string();
  return parent / (DeriveTradeName(stem) + extension);
}

RecorderValidationResult ValidateRecorderOutputPaths(
    const std::filesystem::path& book_ticker_output_path,
    const std::filesystem::path& trade_output_path) {
  if (book_ticker_output_path.empty()) {
    return ValidationFailure("--output must not be empty");
  }
  if (trade_output_path.empty()) {
    return ValidationFailure("--trade-output must not be empty");
  }
  if (const std::optional<std::string> error =
          TerminalSymlinkError(book_ticker_output_path, "--output");
      error.has_value()) {
    return ValidationFailure(*error);
  }
  if (const std::optional<std::string> error =
          TerminalSymlinkError(trade_output_path, "--trade-output");
      error.has_value()) {
    return ValidationFailure(*error);
  }
  if (RecorderSamePath(book_ticker_output_path, trade_output_path)) {
    return ValidationFailure("--output and --trade-output must be different");
  }
  return ValidationSuccess();
}

}  // namespace aquila::tools::market_data
