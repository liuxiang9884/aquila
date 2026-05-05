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
#include <fmt/compile.h>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include "core/config/data_session_config.h"
#include "core/config/instrument_catalog.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/market_data/data_session_config.h"
#include "exchange/gate/market_data/session.h"

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

struct CountingConsumer {
  std::atomic<std::uint64_t> book_tickers{0};

  void OnBookTicker(const aquila::BookTicker&) noexcept {
    book_tickers.fetch_add(1, std::memory_order_relaxed);
  }
};

struct SessionState {
  std::atomic<bool> active{false};
  std::atomic<std::uint8_t> phase{
      static_cast<std::uint8_t>(ws::ConnectionPhase::kDisconnected)};
  std::atomic<std::uint8_t> error{
      static_cast<std::uint8_t>(ws::ConnectionError::kNone)};
};

void HandleState(void* context, ws::ConnectionPhase phase) noexcept {
  auto* state = static_cast<SessionState*>(context);
  state->phase.store(static_cast<std::uint8_t>(phase),
                     std::memory_order_release);
  if (phase == ws::ConnectionPhase::kActive) {
    state->active.store(true, std::memory_order_release);
  }
  fmt::print(stderr, FMT_COMPILE("state={}\n"), magic_enum::enum_name(phase));
}

void HandleError(void* context, ws::ConnectionError error) noexcept {
  auto* state = static_cast<SessionState*>(context);
  state->error.store(static_cast<std::uint8_t>(error),
                     std::memory_order_release);
  fmt::print(stderr, FMT_COMPILE("error={}\n"), magic_enum::enum_name(error));
}

void PrintSettings(
    const aq_gate::GateFutureMarketDataSessionSettings& settings) {
  fmt::print(FMT_COMPILE("name={}\n"), settings.name);
  fmt::print(FMT_COMPILE("websocket host={} service={} target={} tls={} "
                         "bind_cpu_id={}\n"),
             settings.connection.host, settings.connection.service,
             settings.connection.target,
             settings.connection.enable_tls ? "true" : "false",
             settings.connection.runtime_policy.io_cpu_id);
  for (const aq_gate::SymbolBinding& symbol : settings.symbols) {
    fmt::print(FMT_COMPILE("symbol symbol_id={} exchange_symbol={}\n"),
               symbol.symbol_id, symbol.symbol);
  }
}

[[nodiscard]] aq_gate::GateFutureMarketDataSessionSettingsResult LoadSettings(
    const std::filesystem::path& config_path) {
  const config::DataSessionConfigResult config_result =
      config::LoadDataSessionConfigFile(config_path);
  if (!config_result.ok) {
    return {.error = config_result.error};
  }

  const config::InstrumentCatalogLoadResult catalog_result =
      config::LoadInstrumentCatalogFromCsv(
          config_result.config.instrument_catalog.file);
  if (!catalog_result.ok) {
    return {.error = catalog_result.error};
  }

  return aq_gate::BuildGateFutureMarketDataSessionSettings(
      config_result.config, catalog_result.catalog);
}

template <typename TransportSocketT>
int ConnectAndRun(const aq_gate::GateFutureMarketDataSessionSettings& settings,
                  std::uint32_t duration_ms) {
  using Session = aq_gate::FuturesMarketDataSession<
      CountingConsumer, TransportSocketT, aq_gate::FuturesMarketDataDiagnostics,
      ws::DefaultWebSocketOptions,
      aq_gate::FuturesMarketDataSessionDiagnostics>;

  CountingConsumer consumer;
  SessionState state;
  Session session(settings.connection,
                  std::span<const aq_gate::SymbolBinding>(
                      settings.symbols.data(), settings.symbols.size()),
                  consumer);
  session.SetStateHandler(&state, &HandleState);
  session.SetErrorHandler(&state, &HandleError);

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
  const auto phase = static_cast<ws::ConnectionPhase>(
      state.phase.load(std::memory_order_acquire));
  const auto error = static_cast<ws::ConnectionError>(
      state.error.load(std::memory_order_acquire));
  fmt::print(FMT_COMPILE("result={} active={} phase={} error={} "
                         "book_tickers={} rx_messages={} tx_messages={}\n"),
             started.load(std::memory_order_acquire) ? "ok" : "failed",
             state.active.load(std::memory_order_acquire) ? "true" : "false",
             magic_enum::enum_name(phase), magic_enum::enum_name(error),
             consumer.book_tickers.load(std::memory_order_relaxed),
             metrics.rx_messages, metrics.tx_messages);
  return started.load(std::memory_order_acquire) &&
                 state.active.load(std::memory_order_acquire)
             ? 0
             : 1;
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

  aq_gate::GateFutureMarketDataSessionSettingsResult settings_result =
      LoadSettings(config_path);
  if (!settings_result.ok) {
    fmt::print(stderr, FMT_COMPILE("config_error={}\n"), settings_result.error);
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
