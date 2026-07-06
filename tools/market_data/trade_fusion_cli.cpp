#include "tools/market_data/trade_fusion_cli.h"

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

#include "core/common/book_ticker_fusion_metadata_mode.h"
#include "core/config/trade_fusion_config.h"
#include "core/market_data/trade_fusion_runner.h"
#include "core/websocket/runtime_policy.h"
#include "nova/utils/log.h"

namespace aquila::tools::market_data {
namespace {

std::atomic<bool> signal_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void HandleSignal(int signal) {
  (void)signal;
  signal_stop_requested.store(true, std::memory_order_relaxed);
}

bool ApplyAffinity(
    const aquila::market_data::TradeFusionConfig& config) noexcept {
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

[[nodiscard]] const char* FusionMetadataEnabledText() noexcept {
  return aquila::kBookTickerFusionMetadataEnabled ? "true" : "false";
}

[[nodiscard]] std::string FusionMetadataOutputText(
    const aquila::market_data::TradeFusionConfig& config) {
  if constexpr (aquila::kBookTickerFusionMetadataEnabled) {
    return config.output.metadata_bin.string();
  }
  return "disabled";
}

void LogEarlyConfigError(std::string_view error) noexcept {
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

}  // namespace

int RunTradeFusionCli(int argc, char** argv,
                      std::filesystem::path default_config_path,
                      std::string app_description, std::string error_key) {
  std::filesystem::path config_path{std::move(default_config_path)};
  std::uint64_t max_polls{0};

  CLI::App app{std::move(app_description)};
  app.add_option("--config", config_path, "fusion TOML path");
  app.add_option("--max-polls", max_polls, "0 means unlimited");
  CLI11_PARSE(app, argc, argv);

  signal_stop_requested.store(false, std::memory_order_relaxed);

  try {
    const toml::parse_result toml = toml::parse_file(config_path.string());
#if !TOML_EXCEPTIONS
    if (!toml) {
      LogEarlyConfigError(toml.error().description());
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
      auto config_result = aquila::config::ParseTradeFusionConfig(config_toml);
      if (!config_result.ok) {
        NOVA_ERROR("config_error={}", config_result.error);
        return 1;
      }

      const aquila::market_data::TradeFusionConfig& config =
          config_result.value;
      if (!ApplyAffinity(config)) {
        NOVA_WARNING("affinity_warning bind_cpu_id={}", config.bind_cpu_id);
      }

      aquila::market_data::TradeFusionRunner runner(config);
      std::signal(SIGINT, HandleSignal);
      std::signal(SIGTERM, HandleSignal);

      std::uint64_t polls{0};
      while (!signal_stop_requested.load(std::memory_order_relaxed) &&
             (max_polls == 0 || polls < max_polls)) {
        const aquila::market_data::TradeFusionPollStats stats =
            runner.PollOnce();
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
        NOVA_ERROR("flush_error metadata_enabled={} metadata_output={}",
                   FusionMetadataEnabledText(),
                   FusionMetadataOutputText(config));
        return 1;
      }

      NOVA_INFO(
          "result=ok polls={} total_read_count={} total_published_count={} "
          "metadata_enabled={} metadata_write_errors={} metadata_output={}",
          polls, runner.total_read_count(), runner.total_published_count(),
          FusionMetadataEnabledText(), runner.total_metadata_write_errors(),
          FusionMetadataOutputText(config));
      return 0;
    } catch (const std::exception& exc) {
      NOVA_ERROR("{}_error={}", error_key, exc.what());
      return 1;
    }
  } catch (const std::exception& exc) {
    LogEarlyConfigError(exc.what());
    return 1;
  }
}

}  // namespace aquila::tools::market_data
