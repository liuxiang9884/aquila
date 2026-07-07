#ifndef AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_TOOL_SUPPORT_H_
#define AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_TOOL_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <fmt/core.h>

#include "core/common/fusion_metadata_mode.h"
#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_thread.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/trade_fusion_thread.h"
#include "nova/utils/log.h"
#include "tools/market_data/data_fusion_feed.h"

namespace aquila::tools::market_data {

struct BookTickerDataFusionFeedTraits {
  using FusionConfig = aquila::market_data::BookTickerFusionConfig;
  using FusionSourceConfig = aquila::market_data::BookTickerFusionSourceConfig;
  using FusionThreadStats = aquila::market_data::BookTickerFusionThreadStats;

  static constexpr DataFusionFeed kFeed = DataFusionFeed::kBookTicker;

  [[nodiscard]] static std::string_view LaunchShmName(
      const auto& source) noexcept {
    return source.book_ticker_shm_name;
  }

  [[nodiscard]] static std::string_view LaunchChannelName(
      const auto& source) noexcept {
    return source.book_ticker_channel_name;
  }

  [[nodiscard]] static decltype(auto) DataSessionShm(auto& config) noexcept {
    return (config.book_ticker_shm);
  }

  static void SelectFeed(auto* feeds) noexcept {
    feeds->book_ticker = true;
    feeds->trade = false;
  }
};

struct TradeDataFusionFeedTraits {
  using FusionConfig = aquila::market_data::TradeFusionConfig;
  using FusionSourceConfig = aquila::market_data::TradeFusionSourceConfig;
  using FusionThreadStats = aquila::market_data::TradeFusionThreadStats;

  static constexpr DataFusionFeed kFeed = DataFusionFeed::kTrade;

  [[nodiscard]] static std::string_view LaunchShmName(
      const auto& source) noexcept {
    return source.trade_shm_name;
  }

  [[nodiscard]] static std::string_view LaunchChannelName(
      const auto& source) noexcept {
    return source.trade_channel_name;
  }

  [[nodiscard]] static decltype(auto) DataSessionShm(auto& config) noexcept {
    return (config.trade_shm);
  }

  static void SelectFeed(auto* feeds) noexcept {
    feeds->book_ticker = false;
    feeds->trade = true;
  }
};

[[nodiscard]] inline const char* FusionMetadataEnabledText() noexcept {
  return aquila::kFusionMetadataEnabled ? "true" : "false";
}

template <typename FeedTraits>
[[nodiscard]] inline std::string FormatFusionMetadataOutput(
    const typename FeedTraits::FusionConfig& fusion_config) {
  if constexpr (aquila::kFusionMetadataEnabled) {
    return fusion_config.output.metadata_bin.string();
  }
  return "disabled";
}

template <typename FeedTraits>
[[nodiscard]] inline const typename FeedTraits::FusionSourceConfig*
FindFusionSource(const typename FeedTraits::FusionConfig& fusion_config,
                 std::int32_t source_id) {
  for (const typename FeedTraits::FusionSourceConfig& source :
       fusion_config.sources) {
    if (source.source_id == source_id) {
      return &source;
    }
  }
  return nullptr;
}

template <typename LaunchConfig>
[[nodiscard]] bool HasLaunchSource(const LaunchConfig& launch_config,
                                   std::int32_t source_id) {
  for (const auto& launch_source : launch_config.sources) {
    if (launch_source.source_id == source_id) {
      return true;
    }
  }
  return false;
}

template <typename FeedTraits, typename LaunchConfig>
[[nodiscard]] bool ValidateFusionAlignment(
    const LaunchConfig& launch_config,
    const typename FeedTraits::FusionConfig& fusion_config,
    std::string* error) {
  error->clear();
  for (const auto& launch_source : launch_config.sources) {
    const typename FeedTraits::FusionSourceConfig* fusion_source =
        FindFusionSource<FeedTraits>(fusion_config, launch_source.source_id);
    if (fusion_source == nullptr) {
      *error =
          fmt::format("missing fusion source_id={}", launch_source.source_id);
      return false;
    }
    const std::string_view launch_shm =
        FeedTraits::LaunchShmName(launch_source);
    if (std::string_view{fusion_source->shm_name} != launch_shm) {
      *error = fmt::format("source_id={} shm mismatch fusion={} launch={}",
                           launch_source.source_id, fusion_source->shm_name,
                           launch_shm);
      return false;
    }
    const std::string_view launch_channel =
        FeedTraits::LaunchChannelName(launch_source);
    if (std::string_view{fusion_source->channel_name} != launch_channel) {
      *error = fmt::format("source_id={} channel mismatch fusion={} launch={}",
                           launch_source.source_id, fusion_source->channel_name,
                           launch_channel);
      return false;
    }
  }
  for (const typename FeedTraits::FusionSourceConfig& fusion_source :
       fusion_config.sources) {
    if (!HasLaunchSource(launch_config, fusion_source.source_id)) {
      *error = fmt::format("unexpected fusion source_id={}",
                           fusion_source.source_id);
      return false;
    }
  }
  return true;
}

template <typename FeedTraits, typename SourceConfig,
          typename DataSessionConfig>
void ApplyFusionSourceOverride(const SourceConfig& source,
                               DataSessionConfig* data_session_config) {
  data_session_config->name = source.data_session_name;
  auto& shm = FeedTraits::DataSessionShm(*data_session_config);
  shm.enabled = true;
  shm.shm_name = std::string{FeedTraits::LaunchShmName(source)};
  shm.channel_name = std::string{FeedTraits::LaunchChannelName(source)};
  shm.create = true;
  shm.remove_existing = source.remove_existing_source_shm;
  if constexpr (requires { data_session_config->feeds.book_ticker; }) {
    FeedTraits::SelectFeed(&data_session_config->feeds);
  }
  if (source.bind_cpu_id >= 0) {
    data_session_config->connection.runtime_policy.io_cpu_id =
        source.bind_cpu_id;
  }
  data_session_config->diagnostics.latency_outlier.source_id = source.source_id;
}

template <typename PreparedSources>
[[nodiscard]] bool SourcesUseSameTls(const PreparedSources& sources,
                                     std::string_view exchange_name,
                                     std::string* error) {
  error->clear();
  const bool enable_tls =
      sources.front().data_session_config.connection.enable_tls;
  for (const auto& source : sources) {
    if (source.data_session_config.connection.enable_tls != enable_tls) {
      *error = fmt::format(
          "all {} data fusion sources must use the same TLS setting",
          exchange_name);
      return false;
    }
  }
  return true;
}

template <typename FeedTraits, typename LaunchConfig, typename PreparedSources>
void LogDataFusionDryRun(const LaunchConfig& launch_config,
                         const typename FeedTraits::FusionConfig& fusion_config,
                         const PreparedSources& sources) {
  NOVA_INFO(
      "result=ok connect=false feed={} launch={} source_count={} fusion={} "
      "output_shm={} metadata_enabled={} metadata_output={}",
      DataFusionFeedName(FeedTraits::kFeed), launch_config.name, sources.size(),
      fusion_config.name, fusion_config.output.shm_name,
      FusionMetadataEnabledText(),
      FormatFusionMetadataOutput<FeedTraits>(fusion_config));
  for (const auto& source : sources) {
    const auto& connection = source.data_session_config.connection;
    const auto& shm = FeedTraits::DataSessionShm(source.data_session_config);
    NOVA_INFO(
        "source_id={} name={} data_session_config={} shm={} channel={} "
        "tls={} bind_cpu_id={}",
        source.launch_source.source_id, source.data_session_config.name,
        source.launch_source.data_session_config.string(), shm.shm_name,
        shm.channel_name, connection.enable_tls ? "true" : "false",
        connection.runtime_policy.io_cpu_id);
  }
}

template <typename FeedTraits>
void LogDataFusionRunSummary(
    std::string_view launch_name, std::size_t source_count,
    std::uint64_t source_published_count,
    const typename FeedTraits::FusionThreadStats& fusion_stats) {
  const char* result = fusion_stats.ok ? "ok" : "failed";
  if (fusion_stats.ok) {
    NOVA_INFO(
        "result={} feed={} launch={} source_count={} source_published_count={} "
        "fusion_total_read_count={} fusion_total_published_count={} "
        "metadata_enabled={} fusion_metadata_write_errors={} "
        "fusion_flush_ok={} error={}",
        result, DataFusionFeedName(FeedTraits::kFeed), launch_name,
        source_count, source_published_count, fusion_stats.total_read_count,
        fusion_stats.total_published_count, FusionMetadataEnabledText(),
        fusion_stats.total_metadata_write_errors,
        fusion_stats.flush_ok ? "true" : "false", fusion_stats.error);
  } else {
    NOVA_ERROR(
        "result={} feed={} launch={} source_count={} source_published_count={} "
        "fusion_total_read_count={} fusion_total_published_count={} "
        "metadata_enabled={} fusion_metadata_write_errors={} "
        "fusion_flush_ok={} error={}",
        result, DataFusionFeedName(FeedTraits::kFeed), launch_name,
        source_count, source_published_count, fusion_stats.total_read_count,
        fusion_stats.total_published_count, FusionMetadataEnabledText(),
        fusion_stats.total_metadata_write_errors,
        fusion_stats.flush_ok ? "true" : "false", fusion_stats.error);
  }
}

[[nodiscard]] inline nova::LogConfig MakeConsoleOnlyLogConfig(
    std::string_view console_sink_name) {
  nova::LogConfig config;
  config.set_console_sink_name(console_sink_name);
  config.set_file_sink_name("");
  return config;
}

}  // namespace aquila::tools::market_data

#endif  // AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_TOOL_SUPPORT_H_
