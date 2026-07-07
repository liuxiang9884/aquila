#ifndef AQUILA_TOOLS_MARKET_DATA_FUSION_CLI_H_
#define AQUILA_TOOLS_MARKET_DATA_FUSION_CLI_H_

#include <atomic>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <toml++/toml.hpp>

#include "core/common/fusion_metadata_mode.h"
#include "core/config/book_ticker_fusion_config.h"
#include "core/config/trade_fusion_config.h"
#include "core/market_data/fusion/book_ticker.h"
#include "core/market_data/fusion/trade.h"
#include "core/websocket/runtime_policy.h"
#include "nova/utils/log.h"
#include "tools/market_data/data_fusion_feed.h"

namespace aquila::tools::market_data {
namespace internal {

inline std::atomic<bool> fusion_cli_signal_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

inline void HandleFusionCliSignal(int signal) {
  (void)signal;
  fusion_cli_signal_stop_requested.store(true, std::memory_order_relaxed);
}

template <typename Config>
[[nodiscard]] bool ApplyFusionCliAffinity(const Config& config) noexcept {
  if (config.bind_cpu_id < 0) {
    return true;
  }
  aquila::websocket::RuntimePolicy policy;
  policy.affinity_mode = aquila::websocket::AffinityMode::kBestEffort;
  policy.io_cpu_id = config.bind_cpu_id;
  policy.lock_memory = false;
  policy.prefault_stack = true;
  policy.active_spin = true;
  return aquila::websocket::ApplyRuntimePolicy(policy);
}

[[nodiscard]] inline const char* FusionMetadataEnabledText() noexcept {
  return aquila::kFusionMetadataEnabled ? "true" : "false";
}

template <typename Config>
[[nodiscard]] std::string FusionMetadataOutputText(const Config& config) {
  if constexpr (aquila::kFusionMetadataEnabled) {
    return config.output.metadata_bin.string();
  }
  return "disabled";
}

inline void LogEarlyConfigError(std::string_view error) noexcept {
  try {
    nova::LogConfig log_config;
    log_config.set_file_sink_name("");
    log_config.set_json_file_sink_name("");
    nova::InitializeLogging(log_config);
    NOVA_ERROR("config_error={}", error);
    nova::StopLogging();
  } catch (...) {
    nova::StopLogging();
  }
}

}  // namespace internal

struct BookTickerFusionCliTraits {
  using Config = aquila::market_data::BookTickerFusionConfig;
  using Runner = aquila::market_data::BookTickerFusionRunner;

  static constexpr DataFusionFeed kFeed = DataFusionFeed::kBookTicker;

  [[nodiscard]] static aquila::config::BookTickerFusionConfigResult ParseConfig(
      const toml::table& config_toml) {
    return aquila::config::ParseBookTickerFusionConfig(config_toml);
  }
};

struct TradeFusionCliTraits {
  using Config = aquila::market_data::TradeFusionConfig;
  using Runner = aquila::market_data::TradeFusionRunner;

  static constexpr DataFusionFeed kFeed = DataFusionFeed::kTrade;

  [[nodiscard]] static aquila::config::TradeFusionConfigResult ParseConfig(
      const toml::table& config_toml) {
    return aquila::config::ParseTradeFusionConfig(config_toml);
  }
};

template <typename Traits>
[[nodiscard]] int RunFusionCli(int argc, char** argv,
                               std::filesystem::path default_config_path,
                               std::string app_description,
                               std::string error_key) {
  std::filesystem::path config_path{std::move(default_config_path)};
  std::uint64_t max_polls{0};

  CLI::App app{std::move(app_description)};
  app.add_option("--config", config_path, "fusion TOML path");
  app.add_option("--max-polls", max_polls, "0 means unlimited");
  CLI11_PARSE(app, argc, argv);

  internal::fusion_cli_signal_stop_requested.store(false,
                                                   std::memory_order_relaxed);

  try {
    const toml::parse_result toml = toml::parse_file(config_path.string());
#if !TOML_EXCEPTIONS
    if (!toml) {
      internal::LogEarlyConfigError(toml.error().description());
      return 1;
    }
#endif
#if TOML_EXCEPTIONS
    const toml::table& config_toml = toml;
#else
    const toml::table& config_toml = toml.table();
#endif
    nova::LoggingGuard logging_guard{config_toml};

    try {
      auto config_result = Traits::ParseConfig(config_toml);
      if (!config_result.ok) {
        NOVA_ERROR("config_error={}", config_result.error);
        return 1;
      }

      const typename Traits::Config& config = config_result.value;
      if (!internal::ApplyFusionCliAffinity(config)) {
        NOVA_WARNING("affinity_warning bind_cpu_id={}", config.bind_cpu_id);
      }

      typename Traits::Runner runner(config);
      std::signal(SIGINT, internal::HandleFusionCliSignal);
      std::signal(SIGTERM, internal::HandleFusionCliSignal);

      std::uint64_t polls{0};
      while (!internal::fusion_cli_signal_stop_requested.load(
                 std::memory_order_relaxed) &&
             (max_polls == 0 || polls < max_polls)) {
        const auto stats = runner.PollOnce();
        ++polls;
        if (stats.metadata_write_errors != 0) {
          NOVA_ERROR("metadata_write_error count={}",
                     stats.metadata_write_errors);
          return 1;
        }
        if (stats.read_count == 0) {
          std::this_thread::yield();
        }
      }

      if (!runner.Flush()) {
        NOVA_ERROR("flush_error feed={} metadata_enabled={} metadata_output={}",
                   DataFusionFeedName(Traits::kFeed),
                   internal::FusionMetadataEnabledText(),
                   internal::FusionMetadataOutputText(config));
        return 1;
      }

      NOVA_INFO(
          "result=ok feed={} polls={} total_read_count={} "
          "total_published_count={} metadata_enabled={} "
          "metadata_write_errors={} metadata_output={}",
          DataFusionFeedName(Traits::kFeed), polls, runner.total_read_count(),
          runner.total_published_count(), internal::FusionMetadataEnabledText(),
          runner.total_metadata_write_errors(),
          internal::FusionMetadataOutputText(config));
      return 0;
    } catch (const std::exception& exc) {
      NOVA_ERROR("{}_error={}", error_key, exc.what());
      return 1;
    }
  } catch (const std::exception& exc) {
    internal::LogEarlyConfigError(exc.what());
    return 1;
  }
}

}  // namespace aquila::tools::market_data

#endif  // AQUILA_TOOLS_MARKET_DATA_FUSION_CLI_H_
