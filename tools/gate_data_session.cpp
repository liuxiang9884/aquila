#include <cstdint>
#include <filesystem>
#include <string>

#include <CLI/CLI.hpp>
#include <magic_enum/magic_enum.hpp>

#include "core/config/instrument_catalog.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/market_data/data_session.h"
#include "exchange/gate/market_data/data_session_config.h"
#include "nova/utils/log.h"

namespace {

namespace config = aquila::config;
namespace aq_gate = aquila::gate;
namespace ws = aquila::websocket;

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

template <typename SessionT>
void PrintSession(const SessionT& session) {
  const ws::ConnectionConfig& connection = session.connection();
  NOVA_INFO("name={}", session.name());
  NOVA_INFO("websocket host={} service={} target={} tls={} bind_cpu_id={}",
            connection.host, connection.service, connection.target,
            connection.enable_tls ? "true" : "false",
            connection.runtime_policy.io_cpu_id);
  for (const aq_gate::SymbolBinding& symbol : session.symbols()) {
    NOVA_INFO("symbol symbol_id={} exchange_symbol={}", symbol.symbol_id,
              symbol.exchange_symbol);
  }
}

template <typename TransportSocketT>
int CreateAndMaybeRun(const aq_gate::DataSessionConfig& data_session_config,
                      const config::InstrumentCatalog& catalog, bool connect) {
  using Session = aq_gate::DataSession<
      CountingConsumer, TransportSocketT, aq_gate::FuturesMarketDataDiagnostics,
      ws::DefaultWebSocketOptions, aq_gate::DataSessionDiagnostics>;

  CountingConsumer consumer;
  aq_gate::DataSessionCreateResult<
      CountingConsumer, TransportSocketT, aq_gate::FuturesMarketDataDiagnostics,
      ws::DefaultWebSocketOptions, aq_gate::DataSessionDiagnostics>
      session_result =
          aq_gate::CreateDataSession<CountingConsumer, TransportSocketT,
                                     aq_gate::FuturesMarketDataDiagnostics,
                                     ws::DefaultWebSocketOptions,
                                     aq_gate::DataSessionDiagnostics>(
              data_session_config, catalog, consumer);
  if (!session_result.ok) {
    NOVA_ERROR("config_error={}", session_result.error);
    return 1;
  }

  Session& session = *session_result.session;
  PrintSession(session);
  if (!connect) {
    return 0;
  }

  const bool started_ok = session.Run();
  const ws::Metrics metrics = session.SnapshotMetrics();
  const ws::ConnectionPhase phase = session.phase();
  const ws::ConnectionError error = session.last_error();
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
  std::filesystem::path config_path{
      "config/data_sessions/gate_data_session.toml"};
  bool connect{false};

  CLI::App app{"Gate data session"};
  app.add_option("--config", config_path, "data session TOML path");
  app.add_flag("--connect", connect, "connect to the configured websocket");
  CLI11_PARSE(app, argc, argv);

  LoggingGuard logging_guard;

  const aq_gate::DataSessionConfigResult config_result =
      aq_gate::LoadDataSessionConfigFile(config_path);
  if (!config_result.ok) {
    NOVA_ERROR("config_error={}", config_result.error);
    return 1;
  }

  const config::InstrumentCatalogLoadResult catalog_result =
      config::LoadInstrumentCatalogFromCsv(
          config_result.config.instrument_catalog.file);
  if (!catalog_result.ok) {
    NOVA_ERROR("config_error={}", catalog_result.error);
    return 1;
  }

  if (config_result.config.data_session.websocket.endpoint.enable_tls) {
    return CreateAndMaybeRun<ws::TlsSocket>(config_result.config.data_session,
                                            catalog_result.catalog, connect);
  }
  return CreateAndMaybeRun<ws::PlainSocket>(config_result.config.data_session,
                                            catalog_result.catalog, connect);
}
