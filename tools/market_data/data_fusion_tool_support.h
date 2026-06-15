#ifndef AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_TOOL_SUPPORT_H_
#define AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_TOOL_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <fmt/core.h>

#include "core/common/book_ticker_fusion_metadata_mode.h"
#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_thread.h"
#include "nova/utils/log.h"

namespace aquila::tools::market_data {

[[nodiscard]] inline const char* FusionMetadataEnabledText() noexcept {
  return aquila::kBookTickerFusionMetadataEnabled ? "true" : "false";
}

[[nodiscard]] inline std::string FormatFusionMetadataOutput(
    const aquila::market_data::BookTickerFusionConfig& fusion_config) {
  if constexpr (aquila::kBookTickerFusionMetadataEnabled) {
    return fusion_config.output.metadata_bin.string();
  }
  return "disabled";
}

[[nodiscard]] inline const aquila::market_data::BookTickerFusionSourceConfig*
FindFusionSource(
    const aquila::market_data::BookTickerFusionConfig& fusion_config,
    std::int32_t source_id) {
  for (const aquila::market_data::BookTickerFusionSourceConfig& source :
       fusion_config.sources) {
    if (source.source_id == source_id) {
      return &source;
    }
  }
  return nullptr;
}

template <typename LaunchConfig>
[[nodiscard]] bool ValidateBookTickerFusionAlignment(
    const LaunchConfig& launch_config,
    const aquila::market_data::BookTickerFusionConfig& fusion_config,
    std::string* error) {
  error->clear();
  for (const auto& launch_source : launch_config.sources) {
    const aquila::market_data::BookTickerFusionSourceConfig* fusion_source =
        FindFusionSource(fusion_config, launch_source.source_id);
    if (fusion_source == nullptr) {
      *error =
          fmt::format("missing fusion source_id={}", launch_source.source_id);
      return false;
    }
    if (fusion_source->shm_name != launch_source.book_ticker_shm_name) {
      *error = fmt::format("source_id={} shm mismatch fusion={} launch={}",
                           launch_source.source_id, fusion_source->shm_name,
                           launch_source.book_ticker_shm_name);
      return false;
    }
    if (fusion_source->channel_name != launch_source.book_ticker_channel_name) {
      *error = fmt::format("source_id={} channel mismatch fusion={} launch={}",
                           launch_source.source_id, fusion_source->channel_name,
                           launch_source.book_ticker_channel_name);
      return false;
    }
  }
  return true;
}

template <typename SourceConfig, typename DataSessionConfig>
void ApplyBookTickerSourceOverride(const SourceConfig& source,
                                   DataSessionConfig* data_session_config) {
  data_session_config->name = source.data_session_name;
  data_session_config->book_ticker_shm.enabled = true;
  data_session_config->book_ticker_shm.shm_name = source.book_ticker_shm_name;
  data_session_config->book_ticker_shm.channel_name =
      source.book_ticker_channel_name;
  data_session_config->book_ticker_shm.create = true;
  data_session_config->book_ticker_shm.remove_existing =
      source.remove_existing_source_shm;
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

template <typename LaunchConfig, typename PreparedSources>
void LogBookTickerDataFusionDryRun(
    const LaunchConfig& launch_config,
    const aquila::market_data::BookTickerFusionConfig& fusion_config,
    const PreparedSources& sources) {
  NOVA_INFO(
      "result=ok connect=false launch={} source_count={} fusion={} "
      "output_shm={} metadata_enabled={} metadata_output={}",
      launch_config.name, sources.size(), fusion_config.name,
      fusion_config.output.shm_name, FusionMetadataEnabledText(),
      FormatFusionMetadataOutput(fusion_config));
  for (const auto& source : sources) {
    const auto& connection = source.data_session_config.connection;
    NOVA_INFO(
        "source_id={} name={} data_session_config={} shm={} channel={} "
        "tls={} bind_cpu_id={}",
        source.launch_source.source_id, source.data_session_config.name,
        source.launch_source.data_session_config.string(),
        source.data_session_config.book_ticker_shm.shm_name,
        source.data_session_config.book_ticker_shm.channel_name,
        connection.enable_tls ? "true" : "false",
        connection.runtime_policy.io_cpu_id);
  }
}

inline void LogBookTickerDataFusionRunSummary(
    std::string_view launch_name, std::size_t source_count,
    std::uint64_t source_published_count,
    const aquila::market_data::BookTickerFusionThreadStats& fusion_stats) {
  const char* result = fusion_stats.ok ? "ok" : "failed";
  if (fusion_stats.ok) {
    NOVA_INFO(
        "result={} launch={} source_count={} source_published_count={} "
        "fusion_total_read_count={} fusion_total_published_count={} "
        "metadata_enabled={} fusion_metadata_write_errors={} "
        "fusion_flush_ok={} error={}",
        result, launch_name, source_count, source_published_count,
        fusion_stats.total_read_count, fusion_stats.total_published_count,
        FusionMetadataEnabledText(), fusion_stats.total_metadata_write_errors,
        fusion_stats.flush_ok ? "true" : "false", fusion_stats.error);
  } else {
    NOVA_ERROR(
        "result={} launch={} source_count={} source_published_count={} "
        "fusion_total_read_count={} fusion_total_published_count={} "
        "metadata_enabled={} fusion_metadata_write_errors={} "
        "fusion_flush_ok={} error={}",
        result, launch_name, source_count, source_published_count,
        fusion_stats.total_read_count, fusion_stats.total_published_count,
        FusionMetadataEnabledText(), fusion_stats.total_metadata_write_errors,
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
