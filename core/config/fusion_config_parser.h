#ifndef AQUILA_CORE_CONFIG_FUSION_CONFIG_PARSER_H_
#define AQUILA_CORE_CONFIG_FUSION_CONFIG_PARSER_H_

#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <toml++/toml.hpp>

#include "core/common/fusion_metadata_mode.h"
#include "core/websocket/runtime_policy.h"

namespace aquila::config {
namespace internal {

template <typename Traits>
[[nodiscard]] typename Traits::Result Failure(std::string error) {
  typename Traits::Result result;
  result.error = std::move(error);
  return result;
}

template <typename Traits>
[[nodiscard]] typename Traits::Result Success(typename Traits::Config config) {
  typename Traits::Result result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

template <typename Traits>
class FusionConfigParser {
 public:
  explicit FusionConfigParser(const toml::table& node) : node_(node) {}

  [[nodiscard]] typename Traits::Result Parse() {
    ParseFusion();
    if (!ok_) {
      return Failure<Traits>(std::move(error_));
    }
    ParseOutput();
    if (!ok_) {
      return Failure<Traits>(std::move(error_));
    }
    ParseSources();
    if (!ok_) {
      return Failure<Traits>(std::move(error_));
    }
    return Success<Traits>(std::move(config_));
  }

 private:
  [[nodiscard]] std::string RequiredString(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::string> value = value_node.value<std::string>();
    if (!value) {
      if (value_node.node() != nullptr) {
        Fail(name, " must be a string");
        return {};
      }
      Fail(name, " is required");
      return {};
    }
    if (value->empty()) {
      Fail(name, " is required");
      return {};
    }
    return *value;
  }

  [[nodiscard]] std::string OptionalString(
      toml::node_view<const toml::node> value_node,
      std::string fallback, std::string_view name) {
    const std::optional<std::string> value = value_node.value<std::string>();
    if (value) {
      return *value;
    }
    if (value_node.node() != nullptr) {
      Fail(name, " must be a string");
    }
    return fallback;
  }

  [[nodiscard]] bool BoolOr(toml::node_view<const toml::node> value_node,
                            bool fallback, std::string_view name) {
    const std::optional<bool> value = value_node.value<bool>();
    if (value) {
      return *value;
    }
    if (value_node.node() != nullptr) {
      Fail(name, " must be a bool");
    }
    return fallback;
  }

  [[nodiscard]] std::int32_t Int32Or(
      toml::node_view<const toml::node> value_node,
      std::int32_t fallback, std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      if (value_node.node() != nullptr) {
        Fail(name, " must be an integer");
      }
      return fallback;
    }
    if (*value < std::numeric_limits<std::int32_t>::min() ||
        *value > std::numeric_limits<std::int32_t>::max()) {
      Fail(name, " must fit int32");
      return fallback;
    }
    return static_cast<std::int32_t>(*value);
  }

  [[nodiscard]] std::uint32_t PositiveUInt32Or(
      toml::node_view<const toml::node> value_node, std::uint32_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      if (value_node.node() != nullptr) {
        Fail(name, " must be an integer");
      }
      return fallback;
    }
    if (*value <= 0) {
      Fail(name, " must be positive");
      return fallback;
    }
    if (*value >
        static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
      Fail(name, " exceeds uint32 max");
      return fallback;
    }
    return static_cast<std::uint32_t>(*value);
  }

  void ParseFusion() {
    const toml::node_view<const toml::node> fusion = node_["fusion"];
    config_.name = RequiredString(fusion["name"], "fusion.name");
    if (!ok_) {
      return;
    }
    config_.max_events_per_source = PositiveUInt32Or(
        fusion["max_events_per_source"], config_.max_events_per_source,
        "fusion.max_events_per_source");
    if (!ok_) {
      return;
    }
    config_.bind_cpu_id = Int32Or(fusion["bind_cpu_id"],
                                  config_.bind_cpu_id, "fusion.bind_cpu_id");
    if (!ok_) {
      return;
    }
    if (config_.bind_cpu_id < -1) {
      Fail("fusion.bind_cpu_id", " must be -1 or non-negative");
      return;
    }
    if (config_.bind_cpu_id >= 0 &&
        !websocket::CpuIdWithinCpuSetSize(config_.bind_cpu_id)) {
      Fail("fusion.bind_cpu_id", " must be less than CPU_SETSIZE");
      return;
    }
    config_.max_symbol_id = PositiveUInt32Or(
        fusion["max_symbol_id"], config_.max_symbol_id, "fusion.max_symbol_id");
  }

  void ParseOutput() {
    const toml::node_view<const toml::node> output = node_["fusion"]["output"];
    config_.output.shm_name =
        RequiredString(output["shm_name"], "fusion.output.shm_name");
    if (!ok_) {
      return;
    }
    config_.output.channel_name =
        RequiredString(output["channel_name"], "fusion.output.channel_name");
    if (!ok_) {
      return;
    }
    config_.output.remove_existing =
        BoolOr(output["remove_existing"], config_.output.remove_existing,
               "fusion.output.remove_existing");
    if (!ok_) {
      return;
    }
    if constexpr (aquila::kFusionMetadataEnabled) {
      const std::string metadata_bin =
          RequiredString(output["metadata_bin"], "fusion.output.metadata_bin");
      if (!ok_) {
        return;
      }
      config_.output.metadata_bin = metadata_bin;
    } else {
      config_.output.metadata_bin =
          OptionalString(output["metadata_bin"], std::string{},
                         "fusion.output.metadata_bin");
      if (!ok_) {
        return;
      }
    }
  }

  void ParseSources() {
    const toml::array* sources = node_["fusion"]["sources"].as_array();
    if (sources == nullptr || sources->empty()) {
      Fail("fusion.sources", " is required");
      return;
    }

    config_.sources.reserve(sources->size());
    for (const toml::node& source_node : *sources) {
      const toml::table* source_table = source_node.as_table();
      if (source_table == nullptr) {
        Fail("fusion.sources", " entries must be tables");
        return;
      }

      typename Traits::SourceConfig source;
      source.source_id = Int32Or((*source_table)["source_id"],
                                 source.source_id,
                                 "fusion.sources.source_id");
      if (!ok_) {
        return;
      }
      if (source.source_id < 0) {
        Fail("fusion.sources.source_id", " must be non-negative");
        return;
      }
      if (HasSourceId(source.source_id)) {
        Fail("fusion.sources.source_id", " must be unique");
        return;
      }

      source.name =
          RequiredString((*source_table)["name"], "fusion.sources.name");
      if (!ok_) {
        return;
      }
      source.shm_name = RequiredString((*source_table)["shm_name"],
                                       "fusion.sources.shm_name");
      if (!ok_) {
        return;
      }
      source.channel_name =
          OptionalString((*source_table)["channel_name"],
                         std::string{Traits::kDefaultSourceChannel},
                         "fusion.sources.channel_name");
      if (!ok_) {
        return;
      }
      if (source.channel_name.empty()) {
        Fail("fusion.sources.channel_name", " must not be empty");
        return;
      }
      config_.sources.push_back(std::move(source));
    }
  }

  [[nodiscard]] bool HasSourceId(std::int32_t source_id) const noexcept {
    for (const typename Traits::SourceConfig& source : config_.sources) {
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
  typename Traits::Config config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace internal

template <typename Traits>
[[nodiscard]] typename Traits::Result ParseFusionConfig(
    const toml::table& node) {
  return internal::FusionConfigParser<Traits>{node}.Parse();
}

template <typename Traits>
[[nodiscard]] typename Traits::Result LoadFusionConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseFusionConfig<Traits>(parsed);
  } catch (const std::exception& exc) {
    return internal::Failure<Traits>(
        std::string{Traits::kLoadErrorPrefix}.append(exc.what()));
  }
}

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_FUSION_CONFIG_PARSER_H_
