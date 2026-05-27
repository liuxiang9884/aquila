#include "exchange/gate/market_data/data_session.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include <CLI/CLI.hpp>
#include <magic_enum/magic_enum.hpp>
#include <toml++/toml.hpp>

#include "core/market_data/data_shm.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/market_data/data_session_config.h"
#include "nova/utils/log.h"

namespace {

namespace aq_gate = aquila::gate;
namespace aq_md = aquila::market_data;
namespace ws = aquila::websocket;

struct CountingDataSink {
  std::uint64_t book_tickers{0};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++book_tickers;
    if (book_tickers % 1000 != 0) {
      return;
    }
    NOVA_INFO(
        "book_ticker count={} id={} symbol_id={} exchange={} exchange_ns={} "
        "local_ns={} bid_price={:.12g} bid_volume={:.12g} "
        "ask_price={:.12g} ask_volume={:.12g}",
        book_tickers, book_ticker.id, book_ticker.symbol_id,
        magic_enum::enum_name(book_ticker.exchange), book_ticker.exchange_ns,
        book_ticker.local_ns, book_ticker.bid_price, book_ticker.bid_volume,
        book_ticker.ask_price, book_ticker.ask_volume);
  }
};

template <typename DataSink>
std::uint64_t PublishedBookTickers(const DataSink& data_sink) {
  if constexpr (requires { data_sink.published_count(); }) {
    return data_sink.published_count();
  } else {
    return data_sink.book_tickers;
  }
}

template <typename SessionT>
void PrintSession(const SessionT& session) {
  const ws::ConnectionConfig& connection = session.connection();
  NOVA_INFO("name={}", session.name());
  NOVA_INFO("websocket host={} port={} target={} tls={} bind_cpu_id={}",
            connection.host, connection.port, connection.target,
            connection.enable_tls ? "true" : "false",
            connection.runtime_policy.io_cpu_id);
  for (const aq_gate::SymbolBinding& symbol : session.symbols()) {
    NOVA_INFO("symbol symbol_id={} exchange_symbol={}", symbol.symbol_id,
              symbol.exchange_symbol);
  }
}

template <typename WebSocketPolicy, typename DataSink>
int RunDataSessionWithSink(aq_gate::DataSessionConfig data_session_config,
                           DataSink& data_sink, bool connect) {
  using Session = aq_gate::DataSession<DataSink, WebSocketPolicy,
                                       aq_gate::DataSessionDiagnosticsPolicy>;

  Session session(std::move(data_session_config), data_sink);
  PrintSession(session);
  if (!connect) {
    return 0;
  }

  const bool started_ok = session.Run();
  const ws::Metrics metrics = session.SnapshotMetrics();
  const ws::ConnectionPhase phase = session.phase();
  const ws::ConnectionError error = session.last_error();
  const bool active = session.ever_active();
  const std::uint64_t book_tickers = PublishedBookTickers(data_sink);
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

template <typename WebSocketPolicy>
int RunDataSession(aq_gate::DataSessionConfig data_session_config,
                   bool connect) {
  if (data_session_config.book_ticker_shm.enabled) {
    aq_md::DataShmPublisher data_sink{data_session_config.book_ticker_shm};
    return RunDataSessionWithSink<WebSocketPolicy>(
        std::move(data_session_config), data_sink, connect);
  }

  CountingDataSink data_sink;
  return RunDataSessionWithSink<WebSocketPolicy>(std::move(data_session_config),
                                                 data_sink, connect);
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

  const toml::parse_result toml = toml::parse_file(config_path.string());
  nova::LoggingGuard logging_guard{toml};

  aq_gate::DataSessionConfigResult config_result =
      aq_gate::ParseDataSessionConfig(toml, config_path);
  if (!config_result.ok) {
    NOVA_ERROR("config_error={}", config_result.error);
    return 1;
  }

  if (config_result.value.connection.enable_tls) {
    return RunDataSession<aq_gate::DefaultTlsWebSocketPolicy>(
        std::move(config_result.value), connect);
  }
  return RunDataSession<aq_gate::DefaultPlainWebSocketPolicy>(
      std::move(config_result.value), connect);
}
