#ifndef AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_TOOL_SUPPORT_H_
#define AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_TOOL_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/core.h>

#include "core/common/fusion_metadata_mode.h"
#include "core/market_data/fusion/config.h"
#include "core/market_data/fusion/thread.h"
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
    return source.data_shm_name;
  }

  [[nodiscard]] static std::string_view LaunchChannelName(
      const auto& source) noexcept {
    return source.book_ticker_channel_name;
  }

  [[nodiscard]] static decltype(auto) DataSessionShm(auto& config) noexcept {
    return (config.book_ticker_shm);
  }
};

struct TradeDataFusionFeedTraits {
  using FusionConfig = aquila::market_data::TradeFusionConfig;
  using FusionSourceConfig = aquila::market_data::TradeFusionSourceConfig;
  using FusionThreadStats = aquila::market_data::TradeFusionThreadStats;

  static constexpr DataFusionFeed kFeed = DataFusionFeed::kTrade;

  [[nodiscard]] static std::string_view LaunchShmName(
      const auto& source) noexcept {
    return source.data_shm_name;
  }

  [[nodiscard]] static std::string_view LaunchChannelName(
      const auto& source) noexcept {
    return source.trade_channel_name;
  }

  [[nodiscard]] static decltype(auto) DataSessionShm(auto& config) noexcept {
    return (config.trade_shm);
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

template <typename FeedRange>
[[nodiscard]] bool HasFusionFeed(const FeedRange& feeds,
                                 DataFusionFeed feed) noexcept {
  for (DataFusionFeed configured_feed : feeds) {
    if (configured_feed == feed) {
      return true;
    }
  }
  return false;
}

template <typename SourceConfig, typename DataSessionConfig>
void ApplyFusionSourceOverrides(std::initializer_list<DataFusionFeed> feeds,
                                const SourceConfig& source,
                                DataSessionConfig* data_session_config);

template <typename FeedRange, typename SourceConfig, typename DataSessionConfig>
void ApplyFusionSourceOverrides(const FeedRange& feeds,
                                const SourceConfig& source,
                                DataSessionConfig* data_session_config) {
  const bool book_ticker_enabled =
      HasFusionFeed(feeds, DataFusionFeed::kBookTicker);
  const bool trade_enabled = HasFusionFeed(feeds, DataFusionFeed::kTrade);
  data_session_config->name = source.data_session_name;
  if constexpr (requires { data_session_config->feeds.book_ticker; }) {
    data_session_config->feeds.book_ticker = book_ticker_enabled;
    data_session_config->feeds.trade = trade_enabled;
  }
  if constexpr (requires { data_session_config->data_shm; }) {
    auto& data_shm = data_session_config->data_shm;
    data_shm.enabled = book_ticker_enabled || trade_enabled;
    data_shm.book_ticker_enabled = book_ticker_enabled;
    data_shm.trade_enabled = trade_enabled;
    data_shm.shm_name = source.data_shm_name;
    data_shm.book_ticker_channel_name = source.book_ticker_channel_name;
    data_shm.trade_channel_name = source.trade_channel_name;
    data_shm.create = true;
    data_shm.remove_existing = source.remove_existing_source_shm;
    data_session_config->book_ticker_shm = data_shm.BookTickerConfig();
    data_session_config->trade_shm = data_shm.TradeConfig();
  }
  if (source.bind_cpu_id >= 0) {
    data_session_config->connection.runtime_policy.io_cpu_id =
        source.bind_cpu_id;
  }
  data_session_config->diagnostics.latency_outlier.source_id = source.source_id;
}

template <typename SourceConfig, typename DataSessionConfig>
void ApplyFusionSourceOverrides(std::initializer_list<DataFusionFeed> feeds,
                                const SourceConfig& source,
                                DataSessionConfig* data_session_config) {
  ApplyFusionSourceOverrides<std::initializer_list<DataFusionFeed>,
                             SourceConfig, DataSessionConfig>(
      feeds, source, data_session_config);
}

template <typename LaunchConfig, typename BookTickerFusionConfig,
          typename TradeFusionConfig>
[[nodiscard]] bool ValidateDataFusionCpuBindings(
    const LaunchConfig& launch_config,
    const BookTickerFusionConfig* book_ticker_fusion_config,
    const TradeFusionConfig* trade_fusion_config, std::string* error) {
  error->clear();
  std::vector<std::pair<std::int32_t, std::string>> used_cpus;
  const auto add_cpu = [&used_cpus, error](std::int32_t cpu,
                                           std::string name) -> bool {
    if (cpu < 0) {
      return true;
    }
    for (const auto& [used_cpu, used_name] : used_cpus) {
      if (used_cpu == cpu) {
        *error = fmt::format("cpu binding overlap cpu={} first={} second={}",
                             cpu, used_name, name);
        return false;
      }
    }
    used_cpus.emplace_back(cpu, std::move(name));
    return true;
  };

  for (const auto& source : launch_config.sources) {
    if (!add_cpu(source.bind_cpu_id,
                 fmt::format("source_id={}", source.source_id))) {
      return false;
    }
  }
  if constexpr (requires { launch_config.backend_cpu_affinity; }) {
    if (!add_cpu(launch_config.backend_cpu_affinity, "log_backend")) {
      return false;
    }
  }
  if (book_ticker_fusion_config != nullptr &&
      !add_cpu(book_ticker_fusion_config->bind_cpu_id, "book_ticker_fusion")) {
    return false;
  }
  if (trade_fusion_config != nullptr &&
      !add_cpu(trade_fusion_config->bind_cpu_id, "trade_fusion")) {
    return false;
  }
  return true;
}

[[nodiscard]] inline std::string NormalizeFusionShmNameForCompare(
    std::string_view shm_name) {
  if (shm_name.empty() || shm_name.front() == '/') {
    return std::string{shm_name};
  }
  std::string normalized{"/"};
  normalized.append(shm_name);
  return normalized;
}

template <typename LaunchConfig, typename PreparedSources,
          typename BookTickerFusionConfig, typename TradeFusionConfig>
[[nodiscard]] bool ValidatePreparedDataFusionCpuBindings(
    const LaunchConfig& launch_config, const PreparedSources& sources,
    const BookTickerFusionConfig* book_ticker_fusion_config,
    const TradeFusionConfig* trade_fusion_config, std::string* error) {
  error->clear();
  std::vector<std::pair<std::int32_t, std::string>> used_cpus;
  const auto add_cpu = [&used_cpus, error](std::int32_t cpu,
                                           std::string name) -> bool {
    if (cpu < 0) {
      return true;
    }
    for (const auto& [used_cpu, used_name] : used_cpus) {
      if (used_cpu == cpu) {
        *error = fmt::format("cpu binding overlap cpu={} first={} second={}",
                             cpu, used_name, name);
        return false;
      }
    }
    used_cpus.emplace_back(cpu, std::move(name));
    return true;
  };

  for (const auto& source : sources) {
    const std::int32_t cpu =
        source.data_session_config.connection.runtime_policy.io_cpu_id;
    if (!add_cpu(cpu,
                 fmt::format("source_id={}", source.launch_source.source_id))) {
      return false;
    }
  }
  if constexpr (requires { launch_config.backend_cpu_affinity; }) {
    if (!add_cpu(launch_config.backend_cpu_affinity, "log_backend")) {
      return false;
    }
  }
  if (book_ticker_fusion_config != nullptr &&
      !add_cpu(book_ticker_fusion_config->bind_cpu_id, "book_ticker_fusion")) {
    return false;
  }
  if (trade_fusion_config != nullptr &&
      !add_cpu(trade_fusion_config->bind_cpu_id, "trade_fusion")) {
    return false;
  }
  return true;
}

template <typename LaunchConfig, typename BookTickerFusionConfig,
          typename TradeFusionConfig>
[[nodiscard]] bool ValidateDataFusionShmNames(
    const LaunchConfig& launch_config,
    const BookTickerFusionConfig* book_ticker_fusion_config,
    const TradeFusionConfig* trade_fusion_config, std::string* error) {
  error->clear();
  std::vector<std::pair<std::string, std::string>> used_shms;
  const auto add_shm = [&used_shms, error](std::string_view shm_name,
                                           std::string name) -> bool {
    const std::string normalized = NormalizeFusionShmNameForCompare(shm_name);
    for (const auto& [used_shm, used_name] : used_shms) {
      if (used_shm == normalized) {
        *error = fmt::format("data fusion shm overlap shm={} first={} second={}",
                             normalized, used_name, name);
        return false;
      }
    }
    used_shms.emplace_back(normalized, std::move(name));
    return true;
  };

  for (const auto& source : launch_config.sources) {
    if (!add_shm(source.data_shm_name,
                 fmt::format("source_id={}", source.source_id))) {
      return false;
    }
  }
  if (book_ticker_fusion_config != nullptr &&
      !add_shm(book_ticker_fusion_config->output.shm_name,
               "book_ticker_fusion_output")) {
    return false;
  }
  if (trade_fusion_config != nullptr &&
      !add_shm(trade_fusion_config->output.shm_name, "trade_fusion_output")) {
    return false;
  }
  return true;
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
