#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <magic_enum/magic_enum.hpp>

#include "core/config/instrument_catalog.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/market_data/data_session_config.h"
#include "exchange/gate/market_data/session.h"
#include "nova/utils/log.h"

namespace {

namespace config = aquila::config;
namespace aq_gate = aquila::gate;
namespace ws = aquila::websocket;

volatile std::sig_atomic_t g_stop_requested = 0;

void RequestStop(int) noexcept {
  g_stop_requested = 1;
}

void InstallStopHandlers() noexcept {
  std::signal(SIGINT, &RequestStop);
  std::signal(SIGTERM, &RequestStop);
}

struct LoggingGuard {
  LoggingGuard() {
    nova::InitializeLogging();
  }

  ~LoggingGuard() {
    nova::StopLogging();
  }
};

struct CountingConsumer {
  std::uint64_t book_tickers{0};

  void OnBookTicker(const aquila::BookTicker&) noexcept {
    ++book_tickers;
  }
};

void PrintSettings(const aq_gate::FuturesMarketDataSessionSettings& settings) {
  NOVA_INFO("name={}", settings.name);
  NOVA_INFO("websocket host={} service={} target={} tls={} bind_cpu_id={}",
            settings.connection.host, settings.connection.service,
            settings.connection.target,
            settings.connection.enable_tls ? "true" : "false",
            settings.connection.runtime_policy.io_cpu_id);
  for (const aq_gate::SymbolBinding& symbol : settings.symbols) {
    NOVA_INFO("symbol symbol_id={} exchange_symbol={}", symbol.symbol_id,
              symbol.exchange_symbol);
  }
}

[[nodiscard]] aq_gate::FuturesMarketDataSessionSettingsResult LoadSettings(
    const std::filesystem::path& config_path) {
  const aq_gate::FuturesMarketDataConfigResult config_result =
      aq_gate::LoadFuturesMarketDataConfigFile(config_path);
  if (!config_result.ok) {
    return {.error = config_result.error};
  }

  const config::InstrumentCatalogLoadResult catalog_result =
      config::LoadInstrumentCatalogFromCsv(
          config_result.config.instrument_catalog.file);
  if (!catalog_result.ok) {
    return {.error = catalog_result.error};
  }

  return aq_gate::BuildFuturesMarketDataSessionSettings(config_result.config,
                                                        catalog_result.catalog);
}

template <typename TransportSocketT>
int ConnectAndRun(const aq_gate::FuturesMarketDataSessionSettings& settings,
                  std::uint32_t duration_ms) {
  using Session = aq_gate::FuturesMarketDataSession<
      CountingConsumer, TransportSocketT, aq_gate::FuturesMarketDataDiagnostics,
      ws::DefaultWebSocketOptions,
      aq_gate::FuturesMarketDataSessionDiagnostics>;

  CountingConsumer consumer;
  Session session(settings.connection,
                  std::span<const aq_gate::SymbolBinding>(
                      settings.symbols.data(), settings.symbols.size()),
                  consumer);

  std::atomic<bool> done{false};
  std::atomic<bool> started{false};
  std::thread worker([&]() {
    started.store(session.Start(), std::memory_order_release);
    done.store(true, std::memory_order_release);
  });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
  while (std::chrono::steady_clock::now() < deadline && !done.load() &&
         g_stop_requested == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  session.Stop();
  if (worker.joinable()) {
    worker.join();
  }

  const ws::Metrics metrics = session.SnapshotMetrics();
  const ws::ConnectionPhase phase = session.phase();
  const ws::ConnectionError error = session.last_error();
  const bool started_ok = started.load(std::memory_order_acquire);
  const bool active = session.ever_active();
  const std::uint64_t book_tickers = consumer.book_tickers;
  if (started_ok && active) {
    NOVA_INFO(
        "result=ok active=true phase={} error={} book_tickers={} "
        "rx_messages={} tx_messages={}",
        magic_enum::enum_name(phase), magic_enum::enum_name(error),
        book_tickers, metrics.rx_messages, metrics.tx_messages);
    return 0;
  }

  NOVA_WARNING(
      "result=failed active={} phase={} error={} book_tickers={} "
      "rx_messages={} tx_messages={}",
      active ? "true" : "false", magic_enum::enum_name(phase),
      magic_enum::enum_name(error), book_tickers, metrics.rx_messages,
      metrics.tx_messages);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  InstallStopHandlers();

  std::filesystem::path config_path{
      "config/data_sessions/gate_future_market_data.toml"};
  std::uint32_t duration_ms{10'000};
  bool connect{false};

  CLI::App app{"Gate futures market data session"};
  app.add_option("--config", config_path, "data session TOML path");
  app.add_option("--duration-ms", duration_ms, "connect duration");
  app.add_flag("--connect", connect, "connect to the configured websocket");
  CLI11_PARSE(app, argc, argv);

  LoggingGuard logging_guard;

  aq_gate::FuturesMarketDataSessionSettingsResult settings_result =
      LoadSettings(config_path);
  if (!settings_result.ok) {
    NOVA_ERROR("config_error={}", settings_result.error);
    return 1;
  }

  PrintSettings(settings_result.settings);
  if (!connect) {
    return 0;
  }

  if (settings_result.settings.connection.enable_tls) {
    return ConnectAndRun<ws::TlsSocket>(settings_result.settings, duration_ms);
  }
  return ConnectAndRun<ws::PlainSocket>(settings_result.settings, duration_ms);
}
