#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <magic_enum/magic_enum.hpp>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/market_data/data_reader.h"
#include "nova/utils/log.h"

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

struct ProbeHandler {
  explicit ProbeHandler(std::uint64_t log_every) noexcept
      : log_every_(log_every) {}

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++book_tickers_;
    if (log_every_ == 0 ||
        (book_tickers_ != 1 && book_tickers_ % log_every_ != 0)) {
      return;
    }
    NOVA_INFO(
        "book_ticker count={} id={} symbol_id={} exchange={} exchange_ns={} "
        "local_ns={} bid_price={:.12g} bid_volume={:.12g} ask_price={:.12g} "
        "ask_volume={:.12g}",
        book_tickers_, book_ticker.id, book_ticker.symbol_id,
        magic_enum::enum_name(book_ticker.exchange), book_ticker.exchange_ns,
        book_ticker.local_ns, book_ticker.bid_price, book_ticker.bid_volume,
        book_ticker.ask_price, book_ticker.ask_volume);
  }

  [[nodiscard]] std::uint64_t book_tickers() const noexcept {
    return book_tickers_;
  }

 private:
  std::uint64_t log_every_{1000};
  std::uint64_t book_tickers_{0};
};

std::vector<SourceLabel> BuildSourceLabels(
    const aquila::config::DataReaderConfig& config) {
  std::vector<SourceLabel> labels;
  labels.reserve(config.sources.size());
  for (const aquila::config::DataReaderSourceConfig& source : config.sources) {
    labels.push_back(SourceLabel{.name = source.name,
                                 .exchange = source.exchange});
  }
  return labels;
}

void LogSourceStats(
    std::span<const SourceLabel> labels,
    std::span<const aquila::market_data::DataReaderSourceStats> stats) {
  for (std::size_t i = 0; i < stats.size(); ++i) {
    const SourceLabel* label = i < labels.size() ? &labels[i] : nullptr;
    std::string_view name{"unknown"};
    std::string_view exchange{"unknown"};
    if (label != nullptr) {
      name = label->name;
      exchange = magic_enum::enum_name(label->exchange);
    }
    NOVA_INFO(
        "source index={} name={} exchange={} book_tickers={} skipped={} "
        "overruns={} last_book_ticker_id={}",
        i, name, exchange, stats[i].book_tickers, stats[i].skipped,
        stats[i].overruns, stats[i].last_book_ticker_id);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/data_readers/strategy_data_reader.toml"};
  std::uint64_t max_polls{0};
  std::uint64_t log_every{1000};

  CLI::App app{"Data reader probe"};
  app.add_option("--config", config_path, "data reader TOML path");
  app.add_option("--max-polls", max_polls, "0 means unlimited");
  app.add_option("--log-every", log_every,
                 "log first book ticker and then every N book tickers; 0 disables");
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::parse_result toml = toml::parse_file(config_path.string());
    nova::LogConfig log_config;
    log_config.FromToml(toml["log"]);
    nova::InitializeLogging(log_config);

    const auto config_result =
        aquila::config::ParseDataReaderConfig(toml, config_path);
    if (!config_result.ok) {
      NOVA_ERROR("config_error={}", config_result.error);
      nova::StopLogging();
      return 1;
    }

    const std::vector<SourceLabel> source_labels =
        BuildSourceLabels(config_result.value);
    using Reader = aquila::market_data::DataReader<
        aquila::market_data::DataReaderDiagnostics>;
    Reader reader(std::move(config_result.value));
    ProbeHandler handler(log_every);

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::uint64_t polls{0};
    while (!signal_stop_requested.load(std::memory_order_relaxed) &&
           (max_polls == 0 || polls < max_polls)) {
      const std::uint64_t handled = reader.Poll(handler);
      ++polls;
      if (handled == 0) {
        std::this_thread::yield();
      }
    }

    const auto& stats = reader.diagnostics().stats();
    NOVA_INFO(
        "result=ok polls={} handler_book_tickers={} diagnostics_book_tickers={} "
        "empty_polls={}",
        polls, handler.book_tickers(), stats.book_tickers, stats.empty_polls);
    LogSourceStats(source_labels, stats.sources);
    nova::StopLogging();
    return 0;
  } catch (const std::exception& exc) {
    if (::nova::kLogManager.logger() != nullptr) {
      NOVA_ERROR("data_reader_probe_error={}", exc.what());
      nova::StopLogging();
    }
    return 1;
  }
}
