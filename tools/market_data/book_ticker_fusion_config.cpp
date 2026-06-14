#include "tools/market_data/book_ticker_fusion_config.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aquila::tools::market_data {
namespace {

[[nodiscard]] BookTickerFusionConfigResult Failure(std::string error) {
  BookTickerFusionConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] BookTickerFusionConfigResult Success(
    BookTickerFusionConfig config) {
  BookTickerFusionConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

class Parser {
 public:
  explicit Parser(const toml::table& node) : node_(node) {}

  [[nodiscard]] BookTickerFusionConfigResult Parse() {
    ParseFusion();
    if (!ok_) {
      return Failure(std::move(error_));
    }
    ParseOutput();
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

  [[nodiscard]] std::uint32_t PositiveUInt32Or(
      toml::node_view<const toml::node> value_node, std::uint32_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
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
    config_.bind_cpu_id =
        Int32Or(fusion["bind_cpu_id"], config_.bind_cpu_id);
    config_.max_symbol_id = PositiveUInt32Or(
        fusion["max_symbol_id"], config_.max_symbol_id,
        "fusion.max_symbol_id");
  }

  void ParseOutput() {
    const toml::node_view<const toml::node> output =
        node_["fusion"]["output"];
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
        BoolOr(output["remove_existing"], config_.output.remove_existing);
    const std::string metadata_bin =
        RequiredString(output["metadata_bin"], "fusion.output.metadata_bin");
    if (!ok_) {
      return;
    }
    config_.output.metadata_bin = metadata_bin;
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

      BookTickerFusionSourceConfig source;
      source.source_id =
          Int32Or((*source_table)["source_id"], source.source_id);
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
          OptionalString((*source_table)["channel_name"], "book_ticker_channel");
      if (source.channel_name.empty()) {
        Fail("fusion.sources.channel_name", " must not be empty");
        return;
      }
      config_.sources.push_back(std::move(source));
    }
  }

  [[nodiscard]] bool HasSourceId(std::int32_t source_id) const noexcept {
    for (const BookTickerFusionSourceConfig& source : config_.sources) {
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
  BookTickerFusionConfig config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

BookTickerFusionConfigResult ParseBookTickerFusionConfig(
    const toml::table& node) {
  return Parser{node}.Parse();
}

BookTickerFusionConfigResult LoadBookTickerFusionConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseBookTickerFusionConfig(parsed);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load fusion config: "} + exc.what());
  }
}

}  // namespace aquila::tools::market_data
