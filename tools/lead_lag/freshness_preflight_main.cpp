#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/market_data/realtime_data_reader.h"
#include "core/market_data/types.h"
#include "nova/utils/log.h"
#include "tools/lead_lag/freshness_preflight.h"

namespace {

namespace config = aquila::config;
namespace market_data = aquila::market_data;
namespace preflight = aquila::tools::leadlag;

std::atomic<bool> signal_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void HandleSignal(int signal) {
  (void)signal;
  signal_stop_requested.store(true, std::memory_order_relaxed);
}

struct CliOptions {
  std::filesystem::path data_reader_config_path;
  std::filesystem::path lead_lag_config_in;
  std::filesystem::path lead_lag_config_out;
  std::filesystem::path summary_json;
  std::uint64_t duration_sec{60};
  std::uint64_t drain_budget{0};
};

[[nodiscard]] bool WriteTextFile(const std::filesystem::path& path,
                                 const std::string& text, std::string* error) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output.is_open()) {
    if (error != nullptr) {
      *error = fmt::format("failed to open output path {}", path.string());
    }
    return false;
  }
  output << text;
  if (!output.good()) {
    if (error != nullptr) {
      *error = fmt::format("failed to write output path {}", path.string());
    }
    return false;
  }
  return true;
}

void PrepareRealtimePreflightConfig(config::DataReaderConfig* data_reader) {
  for (config::DataReaderSourceConfig& source : data_reader->sources) {
    source.start_position = config::DataReaderStartPosition::kLatest;
    source.read_mode = config::DataReaderReadMode::kDrain;
  }
}

[[nodiscard]] std::uint64_t EffectiveDrainBudget(
    const CliOptions& options, const config::DataReaderConfig& data_reader) {
  if (options.drain_budget != 0) {
    return options.drain_budget;
  }
  return data_reader.max_events_per_drain == 0
             ? 1
             : data_reader.max_events_per_drain;
}

[[nodiscard]] bool MaybeWriteLeadLagConfig(
    const CliOptions& options,
    const std::vector<preflight::FreshnessGroupSummary>& summaries) {
  if (options.lead_lag_config_out.empty()) {
    return true;
  }
  if (options.lead_lag_config_in.empty()) {
    fmt::print(stderr,
               "config_error=--lead-lag-config-in is required when "
               "--lead-lag-config-out is set\n");
    return false;
  }

  try {
    toml::table config = toml::parse_file(options.lead_lag_config_in.string());
    std::string error;
    if (!preflight::ApplyFreshnessThresholdsToLeadLagConfig(&config, summaries,
                                                            &error)) {
      fmt::print(stderr, "config_error={}\n", error);
      return false;
    }
    std::ofstream output(options.lead_lag_config_out,
                         std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
      fmt::print(stderr, "config_error=failed to open lead_lag_config_out={}\n",
                 options.lead_lag_config_out.string());
      return false;
    }
    output << config;
    if (!output.good()) {
      fmt::print(stderr,
                 "config_error=failed to write lead_lag_config_out={}\n",
                 options.lead_lag_config_out.string());
      return false;
    }
  } catch (const std::exception& exc) {
    fmt::print(stderr, "config_error={}\n", exc.what());
    return false;
  }
  return true;
}

[[nodiscard]] int Run(const CliOptions& options) {
  try {
    const toml::table toml =
        toml::parse_file(options.data_reader_config_path.string());
    nova::LoggingGuard logging_guard{toml};

    auto config_result =
        config::ParseDataReaderConfig(toml, options.data_reader_config_path);
    if (!config_result.ok) {
      fmt::print(stderr, "config_error={}\n", config_result.error);
      return 1;
    }

    config::DataReaderConfig data_reader_config =
        std::move(config_result.value);
    PrepareRealtimePreflightConfig(&data_reader_config);
    const std::uint64_t drain_budget =
        EffectiveDrainBudget(options, data_reader_config);

    if (options.lead_lag_config_in.empty()) {
      fmt::print(stderr,
                 "config_error=--lead-lag-config-in is required for "
                 "lead/lag freshness sampling\n");
      return 1;
    }
    const toml::table lead_lag_config =
        toml::parse_file(options.lead_lag_config_in.string());
    std::string pair_config_error;
    std::optional<std::vector<preflight::FreshnessPairConfig>> pair_configs =
        preflight::BuildFreshnessPairConfigsFromLeadLagConfig(
            lead_lag_config, &pair_config_error);
    if (!pair_configs.has_value()) {
      fmt::print(stderr, "config_error={}\n", pair_config_error);
      return 1;
    }

    using Reader = market_data::RealtimeDataReader<
        market_data::RealtimeDataReaderDiagnostics>;
    Reader reader(std::move(data_reader_config));
    preflight::FreshnessPreflightCollector collector(std::move(*pair_configs));

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(options.duration_sec);
    std::uint64_t polls{0};
    std::uint64_t handled_total{0};
    while (!signal_stop_requested.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
      const std::uint64_t handled = reader.Drain(collector, drain_budget);
      ++polls;
      handled_total += handled;
      if (handled == 0) {
        std::this_thread::yield();
      }
    }

    const std::vector<preflight::FreshnessGroupSummary> summaries =
        collector.BuildSummaries();
    if (summaries.empty()) {
      fmt::print(stderr, "config_error=no non-negative freshness samples\n");
      return 1;
    }

    const std::string summary_json = preflight::RenderSummaryJson(summaries);
    if (!options.summary_json.empty()) {
      std::string error;
      if (!WriteTextFile(options.summary_json, summary_json, &error)) {
        fmt::print(stderr, "config_error={}\n", error);
        return 1;
      }
    } else {
      fmt::print("{}", summary_json);
    }

    if (!MaybeWriteLeadLagConfig(options, summaries)) {
      return 1;
    }

    const auto& stats = reader.diagnostics().stats();
    NOVA_INFO(
        "lead_lag_freshness_preflight result=ok duration_sec={} polls={} "
        "handled={} diagnostics_total_count={} groups={} summary_json={} "
        "lead_lag_config_out={}",
        options.duration_sec, polls, handled_total, stats.total_count,
        summaries.size(),
        options.summary_json.empty() ? "-" : options.summary_json.string(),
        options.lead_lag_config_out.empty()
            ? "-"
            : options.lead_lag_config_out.string());
    return 0;
  } catch (const std::exception& exc) {
    fmt::print(stderr, "config_error={}\n", exc.what());
    return 1;
  }
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;

  CLI::App app{"LeadLag freshness preflight generator"};
  app.add_option("--data-reader-config", options.data_reader_config_path,
                 "data reader TOML path for lead/lag fusion canonical SHM")
      ->required();
  app.add_option("--lead-lag-config-in", options.lead_lag_config_in,
                 "input lead_lag strategy TOML containing pair definitions");
  app.add_option("--lead-lag-config-out", options.lead_lag_config_out,
                 "output lead_lag strategy TOML with generated freshness");
  app.add_option(
      "--summary-json", options.summary_json,
      "optional JSON summary output path; prints to stdout when unset");
  app.add_option("--duration-sec", options.duration_sec,
                 "wall-clock sampling duration in seconds");
  app.add_option(
      "--drain-budget", options.drain_budget,
      "events to drain per loop; 0 uses data_reader.max_events_per_drain");
  CLI11_PARSE(app, argc, argv);

  return Run(options);
}
