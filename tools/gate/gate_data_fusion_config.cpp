#include "tools/gate/gate_data_fusion_config.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aquila::tools::gate {
namespace {

[[nodiscard]] GateDataFusionConfigResult Failure(std::string error) {
  GateDataFusionConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] GateDataFusionConfigResult Success(GateDataFusionConfig config) {
  GateDataFusionConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

class Parser {
 public:
  explicit Parser(const toml::table& node) : node_(node) {}

  [[nodiscard]] GateDataFusionConfigResult Parse() {
    ParseLaunch();
    if (!ok_) {
      return Failure(std::move(error_));
    }
    ParseSources();
    if (!ok_) {
      return Failure(std::move(error_));
    }
    return Success(std::move(config_));
  }

 private:
  [[nodiscard]] std::string RequiredString(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::string> value = value_node.value<std::string>();
    if (!value || value->empty()) {
      Fail(name, " is required");
      return {};
    }
    return *value;
  }

  [[nodiscard]] std::string OptionalString(
      toml::node_view<const toml::node> value_node,
      std::string fallback) const {
    const std::optional<std::string> value = value_node.value<std::string>();
    return value.value_or(std::move(fallback));
  }

  [[nodiscard]] bool BoolOr(toml::node_view<const toml::node> value_node,
                            bool fallback) const {
    const std::optional<bool> value = value_node.value<bool>();
    return value.value_or(fallback);
  }

  [[nodiscard]] std::int32_t Int32Or(
      toml::node_view<const toml::node> value_node,
      std::int32_t fallback) const {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    return static_cast<std::int32_t>(*value);
  }

  void ParseLaunch() {
    const toml::node_view<const toml::node> launch = node_["launch"];
    config_.name = RequiredString(launch["name"], "launch.name");
    if (!ok_) {
      return;
    }
    config_.fusion_config =
        RequiredString(launch["fusion_config"], "launch.fusion_config");
  }

  void ParseSources() {
    const toml::array* sources = node_["launch"]["sources"].as_array();
    if (sources == nullptr || sources->empty()) {
      Fail("launch.sources", " is required");
      return;
    }

    config_.sources.reserve(sources->size());
    for (const toml::node& source_node : *sources) {
      const toml::table* source_table = source_node.as_table();
      if (source_table == nullptr) {
        Fail("launch.sources", " entries must be tables");
        return;
      }

      GateDataFusionSourceConfig source;
      source.source_id =
          Int32Or((*source_table)["source_id"], source.source_id);
      if (source.source_id < 0) {
        Fail("launch.sources.source_id", " must be non-negative");
        return;
      }
      if (HasSourceId(source.source_id)) {
        Fail("launch.sources.source_id", " must be unique");
        return;
      }

      source.data_session_config =
          RequiredString((*source_table)["data_session_config"],
                         "launch.sources.data_session_config");
      if (!ok_) {
        return;
      }
      source.data_session_name =
          RequiredString((*source_table)["data_session_name"],
                         "launch.sources.data_session_name");
      if (!ok_) {
        return;
      }
      source.book_ticker_shm_name =
          RequiredString((*source_table)["book_ticker_shm_name"],
                         "launch.sources.book_ticker_shm_name");
      if (!ok_) {
        return;
      }
      source.book_ticker_channel_name = OptionalString(
          (*source_table)["book_ticker_channel_name"], "book_ticker_channel");
      if (source.book_ticker_channel_name.empty()) {
        Fail("launch.sources.book_ticker_channel_name", " must not be empty");
        return;
      }
      source.remove_existing_source_shm =
          BoolOr((*source_table)["remove_existing_source_shm"],
                 source.remove_existing_source_shm);
      source.bind_cpu_id =
          Int32Or((*source_table)["bind_cpu_id"], source.bind_cpu_id);
      config_.sources.push_back(std::move(source));
    }
  }

  [[nodiscard]] bool HasSourceId(std::int32_t source_id) const noexcept {
    for (const GateDataFusionSourceConfig& source : config_.sources) {
      if (source.source_id == source_id) {
        return true;
      }
    }
    return false;
  }

  void Fail(std::string_view name, std::string_view message) {
    ok_ = false;
    error_.assign(name);
    error_.append(message);
  }

  const toml::table& node_;
  GateDataFusionConfig config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

GateDataFusionConfigResult ParseGateDataFusionConfig(const toml::table& node) {
  return Parser{node}.Parse();
}

GateDataFusionConfigResult LoadGateDataFusionConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseGateDataFusionConfig(parsed);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load Gate data fusion config: "} +
                   exc.what());
  }
}

}  // namespace aquila::tools::gate
