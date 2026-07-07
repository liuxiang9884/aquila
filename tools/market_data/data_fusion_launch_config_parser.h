#ifndef AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_LAUNCH_CONFIG_PARSER_H_
#define AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_LAUNCH_CONFIG_PARSER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "tools/market_data/data_fusion_feed.h"

namespace aquila::tools::market_data {
namespace detail {

template <typename LaunchConfig, typename SourceConfig>
class DataFusionLaunchConfigParser {
 public:
  explicit DataFusionLaunchConfigParser(const toml::table& node)
      : node_(node) {}

  [[nodiscard]] Result<LaunchConfig> Parse() {
    ParseLog();
    if (!ok_) {
      return Failure(std::move(error_));
    }
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
  [[nodiscard]] static Result<LaunchConfig> Failure(std::string error) {
    Result<LaunchConfig> result;
    result.error = std::move(error);
    return result;
  }

  [[nodiscard]] static Result<LaunchConfig> Success(LaunchConfig config) {
    Result<LaunchConfig> result;
    result.value = std::move(config);
    result.ok = true;
    return result;
  }

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

  void ParseLog() {
    const toml::node_view<const toml::node> log = node_["log"];
    config_.backend_cpu_affinity =
        Int32Or(log["backend_cpu_affinity"], config_.backend_cpu_affinity);
  }

  void ParseLaunch() {
    const toml::node_view<const toml::node> launch = node_["launch"];
    config_.name = RequiredString(launch["name"], "launch.name");
    if (!ok_) {
      return;
    }
    ParseFeeds(launch["feeds"], "launch.feeds");
    if (!ok_) {
      return;
    }
    ParseFusionConfigs(launch["fusion_configs"]);
  }

  void ParseFeeds(toml::node_view<const toml::node> value_node,
                  std::string_view name) {
    const toml::array* feeds = value_node.as_array();
    if (feeds == nullptr || feeds->empty()) {
      Fail(name, " must contain at least one feed");
      return;
    }
    config_.feeds.clear();
    for (const toml::node& feed_node : *feeds) {
      const std::optional<std::string> feed = feed_node.value<std::string>();
      if (!feed || feed->empty()) {
        Fail(name, " must contain strings");
        return;
      }
      const DataFusionFeed parsed_feed = ParseRequiredFeed(*feed, name);
      if (!ok_) {
        return;
      }
      if (HasFeed(parsed_feed)) {
        Fail(name, std::string{" contains duplicate feed "} + *feed);
        return;
      }
      config_.feeds.push_back(parsed_feed);
    }
  }

  [[nodiscard]] DataFusionFeed ParseRequiredFeed(std::string_view feed,
                                                 std::string_view name) {
    if (feed == "book_ticker") {
      return DataFusionFeed::kBookTicker;
    }
    if (feed == "trade") {
      return DataFusionFeed::kTrade;
    }
    Fail(name, " must be book_ticker or trade");
    return DataFusionFeed::kBookTicker;
  }

  void ParseFusionConfigs(toml::node_view<const toml::node> fusion_configs) {
    if (!fusion_configs) {
      Fail("launch.fusion_configs", " is required");
      return;
    }
    if (HasFeed(DataFusionFeed::kBookTicker)) {
      config_.book_ticker_fusion_config = RequiredString(
          fusion_configs["book_ticker"], "launch.fusion_configs.book_ticker");
      if (!ok_) {
        return;
      }
    }
    if (HasFeed(DataFusionFeed::kTrade)) {
      config_.trade_fusion_config = RequiredString(
          fusion_configs["trade"], "launch.fusion_configs.trade");
      if (!ok_) {
        return;
      }
    }
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

      SourceConfig source;
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
      source.data_shm_name = RequiredString((*source_table)["data_shm_name"],
                                            "launch.sources.data_shm_name");
      if (!ok_) {
        return;
      }
      if (HasFeed(DataFusionFeed::kBookTicker)) {
        source.book_ticker_channel_name = OptionalString(
            (*source_table)["book_ticker_channel_name"], "book_ticker_channel");
        if (source.book_ticker_channel_name.empty()) {
          Fail("launch.sources.book_ticker_channel_name", " must not be empty");
          return;
        }
      }
      if (HasFeed(DataFusionFeed::kTrade)) {
        source.trade_channel_name = OptionalString(
            (*source_table)["trade_channel_name"], "trade_channel");
        if (source.trade_channel_name.empty()) {
          Fail("launch.sources.trade_channel_name", " must not be empty");
          return;
        }
      }
      source.remove_existing_source_shm =
          BoolOr((*source_table)["remove_existing_source_shm"],
                 source.remove_existing_source_shm);
      source.bind_cpu_id =
          Int32Or((*source_table)["bind_cpu_id"], source.bind_cpu_id);
      config_.sources.push_back(std::move(source));
    }
  }

  [[nodiscard]] bool HasFeed(DataFusionFeed feed) const noexcept {
    for (DataFusionFeed configured_feed : config_.feeds) {
      if (configured_feed == feed) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool HasSourceId(std::int32_t source_id) const noexcept {
    for (const SourceConfig& source : config_.sources) {
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
  LaunchConfig config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace detail

template <typename LaunchConfig, typename SourceConfig>
[[nodiscard]] Result<LaunchConfig> ParseDataFusionLaunchConfig(
    const toml::table& node) {
  return detail::DataFusionLaunchConfigParser<LaunchConfig, SourceConfig>{node}
      .Parse();
}

}  // namespace aquila::tools::market_data

#endif  // AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_LAUNCH_CONFIG_PARSER_H_
