#include "tools/market_data/data_reader_recorder.h"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/market_data/realtime_data_reader.h"
#include "nova/utils/log.h"
#include "tools/market_data/data_reader_recorder_config.h"
#include "tools/market_data/data_reader_tool_logging.h"

namespace {

std::atomic<bool> signal_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void HandleSignal(int signal) {
  (void)signal;
  signal_stop_requested.store(true, std::memory_order_relaxed);
}

struct SourceLabel {
  std::string name;
  aquila::Exchange exchange{aquila::Exchange::kBinance};
};

[[nodiscard]] aquila::tools::market_data::RecorderWriteMode ParseWriteMode(
    std::string_view text) {
  if (text == "truncate") {
    return aquila::tools::market_data::RecorderWriteMode::kTruncate;
  }
  if (text == "append") {
    return aquila::tools::market_data::RecorderWriteMode::kAppend;
  }
  throw std::invalid_argument("mode must be truncate or append");
}

[[nodiscard]] std::string_view WriteModeName(
    aquila::tools::market_data::RecorderWriteMode write_mode) noexcept {
  switch (write_mode) {
    case aquila::tools::market_data::RecorderWriteMode::kTruncate:
      return "truncate";
    case aquila::tools::market_data::RecorderWriteMode::kAppend:
      return "append";
  }
  return "unknown";
}

[[nodiscard]] std::string OptionalNs(const std::optional<std::int64_t>& value) {
  if (!value.has_value()) {
    return "none";
  }
  return fmt::format("{}", *value);
}

[[nodiscard]] std::vector<SourceLabel> BuildSourceLabels(
    const aquila::config::DataReaderConfig& config) {
  std::vector<SourceLabel> labels;
  labels.reserve(config.sources.size());
  for (const aquila::config::DataReaderSourceConfig& source : config.sources) {
    labels.push_back(
        SourceLabel{.name = source.name, .exchange = source.exchange});
  }
  return labels;
}

void LogSourceConfig(const aquila::config::DataReaderConfig& config) {
  for (std::size_t i = 0; i < config.sources.size(); ++i) {
    const aquila::config::DataReaderSourceConfig& source = config.sources[i];
    NOVA_INFO("{}",
              aquila::tools::market_data::FormatSourceConfigLog(i, source));
    if (source.read_mode == aquila::config::DataReaderReadMode::kLatest) {
      NOVA_WARNING(
          "latest_read_mode_source index={} name={} semantics=reader records "
          "only the latest visible BookTicker per source poll and counts "
          "intermediate unread records as skipped",
          i, source.name);
    }
  }
}

void LogSourceStats(
    std::span<const SourceLabel> labels,
    std::span<const aquila::market_data::RealtimeDataReaderSourceStats> stats) {
  for (std::size_t i = 0; i < stats.size(); ++i) {
    const SourceLabel* label = i < labels.size() ? &labels[i] : nullptr;
    std::string_view name{"unknown"};
    std::string_view exchange{"unknown"};
    if (label != nullptr) {
      name = label->name;
      exchange = magic_enum::enum_name(label->exchange);
    }
    NOVA_INFO(
        "source_stats index={} name={} exchange={} book_ticker_count={} "
        "trade_count={} skipped={} overruns={} last_book_ticker_id={} "
        "last_trade_id={}",
        i, name, exchange, stats[i].book_ticker_count, stats[i].trade_count,
        stats[i].skipped, stats[i].overruns, stats[i].last_book_ticker_id,
        stats[i].last_trade_id);
  }
}

void LogRecorderStats(const aquila::tools::market_data::RecorderStats& stats) {
  NOVA_INFO(
      "recorder_stats total_records={} first_exchange_ns={} first_local_ns={} "
      "last_exchange_ns={} last_local_ns={}",
      stats.total_records, OptionalNs(stats.first_exchange_ns),
      OptionalNs(stats.first_local_ns), OptionalNs(stats.last_exchange_ns),
      OptionalNs(stats.last_local_ns));

  for (const aquila::Exchange exchange :
       aquila::tools::market_data::kRecorderTrackedExchanges) {
    NOVA_INFO("exchange_stats exchange={} records={}",
              magic_enum::enum_name(exchange),
              stats.RecordsForExchange(exchange));
  }
}

template <typename Recorder>
[[nodiscard]] std::uint64_t SegmentsCompleted(
    const Recorder& recorder) noexcept {
  if constexpr (requires { recorder.segments_completed(); }) {
    return recorder.segments_completed();
  }
  return 0;
}

void LogRecorderRotationConfig(
    const aquila::tools::market_data::RecorderConfig& config) {
  NOVA_INFO(
      "recorder_rotation enabled={} interval_sec={} output_dir={} "
      "file_prefix={} manifest_path={}",
      config.rotation.enabled ? "true" : "false",
      config.rotation.rotation_interval_sec,
      config.rotation.output_dir.string(), config.rotation.file_prefix,
      config.rotation.manifest_path.string());
}

template <typename Reader, typename Recorder>
int RunRecorderLoop(Reader& reader, Recorder& recorder,
                    std::span<const SourceLabel> source_labels,
                    const std::filesystem::path& output_path,
                    std::uint64_t max_polls, std::uint64_t drain_budget) {
  std::uint64_t polls{0};
  auto log_summary = [&](std::string_view result,
                         std::string_view stop_reason) {
    const auto& reader_stats = reader.diagnostics().stats();
    NOVA_INFO(
        "result={} stop_reason={} polls={} handler_book_tickers={} "
        "diagnostics_total_count={} output={} segments_completed={}",
        result, stop_reason, polls, recorder.stats().total_records,
        reader_stats.total_count, output_path.string(),
        SegmentsCompleted(recorder));
    LogRecorderStats(recorder.stats());
    LogSourceStats(source_labels, reader_stats.sources);
  };

  while (!signal_stop_requested.load(std::memory_order_relaxed) &&
         (max_polls == 0 || polls < max_polls)) {
    const std::uint64_t handled = reader.Drain(recorder, drain_budget);
    ++polls;
    if (recorder.write_error()) {
      log_summary("error", "write_error");
      NOVA_ERROR("recorder_write_error output={}", output_path.string());
      return 1;
    }
    if (handled == 0) {
      std::this_thread::yield();
    }
  }

  if (!recorder.Flush()) {
    log_summary("error", "flush_error");
    NOVA_ERROR("recorder_flush_error output={}", output_path.string());
    return 1;
  }

  const bool stopped_by_signal =
      signal_stop_requested.load(std::memory_order_relaxed);
  const bool stopped_by_max_polls = max_polls != 0 && polls >= max_polls;
  log_summary("ok", stopped_by_signal      ? "signal"
                    : stopped_by_max_polls ? "max_polls"
                                           : "completed");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/data_readers/strategy_data_reader.toml"};
  std::filesystem::path output_path;
  std::string mode_text{"truncate"};
  std::uint64_t max_polls{0};

  CLI::App app{"SHM data reader to merged BookTicker replay binary recorder"};
  app.add_option("--config", config_path, "data reader TOML path");
  app.add_option("--output", output_path,
                 "output BookTicker binary path without header")
      ->required();
  app.add_option("--mode", mode_text, "truncate or append")
      ->check(CLI::IsMember({"truncate", "append"}));
  app.add_option("--max-polls", max_polls, "0 means unlimited");
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::parse_result toml = toml::parse_file(config_path.string());
    nova::LoggingGuard logging_guard{toml};

    try {
      const auto config_result =
          aquila::config::ParseDataReaderConfig(toml, config_path);
      if (!config_result.ok) {
        NOVA_ERROR("config_error={}", config_result.error);
        return 1;
      }

      const std::vector<SourceLabel> source_labels =
          BuildSourceLabels(config_result.value);
      const std::uint64_t drain_budget =
          config_result.value.max_events_per_drain;
      const auto write_mode = ParseWriteMode(mode_text);
      const auto recorder_config_result =
          aquila::tools::market_data::ParseRecorderConfig(toml, output_path,
                                                          write_mode);
      if (!recorder_config_result.ok) {
        NOVA_ERROR("recorder_config_error={}", recorder_config_result.error);
        return 1;
      }

      NOVA_INFO("recorder_write_mode={}", WriteModeName(write_mode));
      NOVA_INFO("{}", aquila::tools::market_data::FormatToolStartupLog(
                          "data_reader_recorder", "realtime", config_path,
                          output_path, max_polls, drain_budget,
                          sizeof(aquila::BookTicker)));
      LogRecorderRotationConfig(recorder_config_result.value);
      LogSourceConfig(config_result.value);

      using Reader = aquila::market_data::RealtimeDataReader<
          aquila::market_data::RealtimeDataReaderDiagnostics>;
      Reader reader(std::move(config_result.value));

      std::signal(SIGINT, HandleSignal);
      std::signal(SIGTERM, HandleSignal);

      if (recorder_config_result.value.rotation.enabled) {
        aquila::tools::market_data::RotatingBookTickerBinaryRecorder recorder(
            recorder_config_result.value.rotation);
        return RunRecorderLoop(reader, recorder, source_labels, output_path,
                               max_polls, drain_budget);
      }

      aquila::tools::market_data::BookTickerBinaryRecorder recorder(output_path,
                                                                    write_mode);
      return RunRecorderLoop(reader, recorder, source_labels, output_path,
                             max_polls, drain_budget);
    } catch (const std::exception& exc) {
      NOVA_ERROR("data_reader_recorder_error={}", exc.what());
      return 1;
    }
  } catch (const std::exception& exc) {
    if (::nova::kLogManager.logger() != nullptr) {
      NOVA_ERROR("data_reader_recorder_error={}", exc.what());
    }
    return 1;
  }
}
