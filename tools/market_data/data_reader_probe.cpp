#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/market_data/data_reader.h"
#include "nova/utils/log.h"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void HandleSignal(int signal) {
  (void)signal;
  g_stop_requested = 1;
}

struct CountingHandler {
  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    (void)book_ticker;
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/data_readers/strategy_data_reader.toml"};
  std::uint64_t max_polls{0};

  CLI::App app{"Data reader probe"};
  app.add_option("--config", config_path, "data reader TOML path");
  app.add_option("--max-polls", max_polls, "0 means unlimited");
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

    using Reader = aquila::market_data::DataReader<
        aquila::market_data::DataReaderDiagnostics>;
    Reader reader(std::move(config_result.value));
    CountingHandler handler;

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::uint64_t polls{0};
    while (g_stop_requested == 0 && (max_polls == 0 || polls < max_polls)) {
      const std::uint64_t handled = reader.Poll(handler);
      ++polls;
      if (handled == 0) {
        std::this_thread::yield();
      }
    }

    const auto& stats = reader.diagnostics().stats();
    NOVA_INFO("result=ok polls={} book_tickers={} empty_polls={}", polls,
              stats.book_tickers, stats.empty_polls);
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
