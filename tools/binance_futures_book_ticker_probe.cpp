#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <fmt/compile.h>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include "core/websocket/websocket_client.h"
#include "exchange/binance/market_data/session.h"
#include <netdb.h>

namespace {

namespace binance = aquila::binance;
namespace ws = aquila::websocket;

volatile std::sig_atomic_t g_stop_requested = 0;

void RequestStop(int) noexcept {
  g_stop_requested = 1;
}

void InstallStopHandlers() noexcept {
  std::signal(SIGINT, &RequestStop);
  std::signal(SIGTERM, &RequestStop);
}

std::string FormatSockaddr(const sockaddr_storage& storage,
                           socklen_t storage_len) {
  char host[NI_MAXHOST]{};
  char service[NI_MAXSERV]{};
  const int rc = ::getnameinfo(
      reinterpret_cast<const sockaddr*>(&storage), storage_len, host,
      sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
  if (rc != 0) {
    return "unavailable";
  }
  return fmt::format(FMT_COMPILE("{}:{}"), host, service);
}

void PrintSocketInfo(int fd) {
  if (fd < 0) {
    fmt::print(stderr, FMT_COMPILE("socket fd=unavailable\n"));
    return;
  }

  sockaddr_storage local{};
  socklen_t local_len = sizeof(local);
  sockaddr_storage peer{};
  socklen_t peer_len = sizeof(peer);
  const bool has_local =
      ::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0;
  const bool has_peer =
      ::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0;

  int incoming_cpu = -1;
  socklen_t incoming_cpu_len = sizeof(incoming_cpu);
  const bool has_incoming_cpu =
      ::getsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &incoming_cpu,
                   &incoming_cpu_len) == 0;

  fmt::print(stderr,
             FMT_COMPILE("socket fd={} local={} peer={} so_incoming_cpu={}\n"),
             fd, has_local ? FormatSockaddr(local, local_len) : "unavailable",
             has_peer ? FormatSockaddr(peer, peer_len) : "unavailable",
             has_incoming_cpu ? fmt::format(FMT_COMPILE("{}"), incoming_cpu)
                              : "unavailable");
}

struct ProbeStats {
  std::uint64_t decoded_book_tickers{0};
  std::uint64_t arrival_gap_samples{0};
  std::uint64_t arrival_gap_total_ns{0};
  std::uint64_t arrival_gap_max_ns{0};
  std::uint64_t first_arrival_ns{0};
  std::uint64_t last_arrival_ns{0};
  aquila::BookTicker first_book_ticker{};
  aquila::BookTicker last_book_ticker{};
  bool has_first_book_ticker{false};
};

void RecordArrivalNs(ProbeStats* stats, std::uint64_t arrival_ns) {
  if (stats->first_arrival_ns == 0) {
    stats->first_arrival_ns = arrival_ns;
  }
  if (stats->last_arrival_ns != 0) {
    const std::uint64_t gap_ns = arrival_ns - stats->last_arrival_ns;
    ++stats->arrival_gap_samples;
    stats->arrival_gap_total_ns += gap_ns;
    stats->arrival_gap_max_ns = std::max(stats->arrival_gap_max_ns, gap_ns);
  }
  stats->last_arrival_ns = arrival_ns;
}

struct ProbeConsumer {
  ProbeStats* stats{nullptr};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    if (stats == nullptr) {
      return;
    }
    if (!stats->has_first_book_ticker) {
      stats->first_book_ticker = book_ticker;
      stats->has_first_book_ticker = true;
    }
    RecordArrivalNs(stats, static_cast<std::uint64_t>(book_ticker.local_ns));
    ++stats->decoded_book_tickers;
    stats->last_book_ticker = book_ticker;
  }
};

struct ProbeConfig {
  std::string host{"fstream.binance.com"};
  std::string port{"443"};
  std::string contract{"BTCUSDT"};
  std::int32_t symbol_id{1};
  std::uint32_t duration_ms{1'800'000};
  int cpu{-1};
  bool tls{true};
};

template <typename TransportSocketT>
class ProbeRunner {
 public:
  explicit ProbeRunner(ProbeConfig config)
      : config_(std::move(config)),
        symbols_{binance::SymbolBinding{.symbol = config_.contract,
                                        .symbol_id = config_.symbol_id}},
        consumer_{.stats = &stats_},
        session_(BuildConnectionConfig(config_), symbols_, consumer_) {}

  void Start() {
    session_.SetStateHandler(this, &ProbeRunner::HandleState);
    session_.SetErrorHandler(this, &ProbeRunner::HandleError);

    thread_ = std::thread([this]() {
      result_.store(session_.Start(), std::memory_order_release);
      metrics_ = session_.SnapshotMetrics();
      done_.store(true, std::memory_order_release);
    });
  }

  void Stop() noexcept {
    session_.Stop();
  }

  void Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] bool done() const noexcept {
    return done_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool result() const noexcept {
    return result_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool saw_active() const noexcept {
    return saw_active_.load(std::memory_order_acquire);
  }

  [[nodiscard]] ws::ConnectionPhase phase() const noexcept {
    return static_cast<ws::ConnectionPhase>(
        phase_.load(std::memory_order_acquire));
  }

  [[nodiscard]] ws::ConnectionError error() const noexcept {
    return static_cast<ws::ConnectionError>(
        error_.load(std::memory_order_acquire));
  }

  [[nodiscard]] const ProbeStats& stats() const noexcept {
    return stats_;
  }

  [[nodiscard]] ws::Metrics metrics() const noexcept {
    return metrics_;
  }

  [[nodiscard]] const ProbeConfig& config() const noexcept {
    return config_;
  }

  [[nodiscard]] std::string_view stream_target() const noexcept {
    return session_.stream_target();
  }

  [[nodiscard]] const binance::FuturesMarketDataSessionStats& session_stats()
      const noexcept {
    return session_.stats();
  }

  [[nodiscard]] const binance::FuturesMarketDataClientStats& market_data_stats()
      const noexcept {
    return session_.market_data_client_diagnostics().stats();
  }

 private:
  using Session = binance::FuturesMarketDataSession<
      ProbeConsumer, TransportSocketT, binance::FuturesMarketDataDiagnostics,
      ws::DefaultWebSocketOptions,
      binance::FuturesMarketDataSessionDiagnostics>;

  static ws::ConnectionConfig BuildConnectionConfig(const ProbeConfig& config) {
    ws::ConnectionConfig connection_config{};
    connection_config.host = config.host;
    connection_config.service = config.port;
    connection_config.enable_tls = TransportSocketT::kUsesTls;
    connection_config.max_reads_per_drive = 8;
    connection_config.read_until_would_block = false;
    connection_config.runtime_policy.io_cpu_id = config.cpu;
    connection_config.runtime_policy.affinity_mode =
        config.cpu >= 0 ? ws::AffinityMode::kBestEffort
                        : ws::AffinityMode::kNone;
    return connection_config;
  }

  static void HandleState(void* context, ws::ConnectionPhase phase) noexcept {
    static_cast<ProbeRunner*>(context)->OnState(phase);
  }

  static void HandleError(void* context, ws::ConnectionError error) noexcept {
    static_cast<ProbeRunner*>(context)->OnError(error);
  }

  void OnState(ws::ConnectionPhase phase) noexcept {
    phase_.store(static_cast<std::uint8_t>(phase), std::memory_order_release);
    fmt::print(stderr, FMT_COMPILE("state={}\n"), magic_enum::enum_name(phase));
    if (phase != ws::ConnectionPhase::kActive) {
      return;
    }

    saw_active_.store(true, std::memory_order_release);
    PrintSocketInfo(session_.NativeFd());
    fmt::print(stderr, FMT_COMPILE("stream_target={}\n"),
               session_.stream_target());
  }

  void OnError(ws::ConnectionError error) noexcept {
    error_.store(static_cast<std::uint8_t>(error), std::memory_order_release);
    fmt::print(stderr, FMT_COMPILE("error={}\n"), magic_enum::enum_name(error));
  }

  ProbeConfig config_;
  std::array<binance::SymbolBinding, 1> symbols_;
  ProbeStats stats_{};
  ProbeConsumer consumer_;
  Session session_;
  std::thread thread_;
  ws::Metrics metrics_{};
  std::atomic<bool> done_{false};
  std::atomic<bool> result_{false};
  std::atomic<bool> saw_active_{false};
  std::atomic<std::uint8_t> phase_{
      static_cast<std::uint8_t>(ws::ConnectionPhase::kDisconnected)};
  std::atomic<std::uint8_t> error_{
      static_cast<std::uint8_t>(ws::ConnectionError::kNone)};
};

double AverageNs(std::uint64_t total, std::uint64_t samples) noexcept {
  if (samples == 0) {
    return 0.0;
  }
  return static_cast<double>(total) / static_cast<double>(samples);
}

template <typename RunnerT>
void PrintSummary(const RunnerT& runner) {
  const ProbeStats& stats = runner.stats();
  const binance::FuturesMarketDataSessionStats& session_stats =
      runner.session_stats();
  const binance::FuturesMarketDataClientStats& market_data_stats =
      runner.market_data_stats();
  const ws::Metrics metrics = runner.metrics();
  const ProbeConfig& config = runner.config();
  const aquila::BookTicker& first = stats.first_book_ticker;
  const aquila::BookTicker& last = stats.last_book_ticker;
  const std::int64_t book_ticker_id_delta =
      stats.has_first_book_ticker ? last.id - first.id : 0;
  const std::uint64_t failed_or_unmapped =
      session_stats.text_messages > stats.decoded_book_tickers
          ? session_stats.text_messages - stats.decoded_book_tickers
          : 0;

  fmt::print(FMT_COMPILE("config host={} port={} target={} tls={} contract={} "
                         "symbol_id={} duration_ms={}\n"),
             config.host, config.port, runner.stream_target(),
             config.tls ? "yes" : "no", config.contract, config.symbol_id,
             config.duration_ms);
  fmt::print(FMT_COMPILE("result={} active={} phase={} error={} rx_messages={} "
                         "rx_bytes={} tx_messages={} tx_bytes={} reconnects={} "
                         "heartbeat_timeouts={}\n"),
             runner.result() ? "ok" : "failed",
             runner.saw_active() ? "yes" : "no",
             magic_enum::enum_name(runner.phase()),
             magic_enum::enum_name(runner.error()), metrics.rx_messages,
             metrics.rx_bytes, metrics.tx_messages, metrics.tx_bytes,
             metrics.reconnects, metrics.heartbeat_timeouts);
  fmt::print(FMT_COMPILE("session text={} binary={} non_final={} control={} "
                         "book_ticker={}\n"),
             session_stats.text_messages, session_stats.binary_messages,
             session_stats.non_final_messages, session_stats.control_messages,
             session_stats.book_ticker_messages);
  fmt::print(FMT_COMPILE("market_data malformed_json={} unknown_symbol={} "
                         "book_ticker={}\n"),
             market_data_stats.malformed_json_messages,
             market_data_stats.unknown_symbols,
             market_data_stats.book_ticker_messages);
  fmt::print(
      FMT_COMPILE("book_ticker payloads={} decoded={} failed_or_unmapped={} "
                  "first_id={} last_id={} id_delta={}\n"),
      session_stats.text_messages, stats.decoded_book_tickers,
      failed_or_unmapped, first.id, last.id, book_ticker_id_delta);
  fmt::print(FMT_COMPILE("arrival_gap_ns samples={} avg={:.2f} max={}\n"),
             stats.arrival_gap_samples,
             AverageNs(stats.arrival_gap_total_ns, stats.arrival_gap_samples),
             stats.arrival_gap_max_ns);
  fmt::print(
      FMT_COMPILE("last_book_ticker id={} symbol_id={} exchange={} "
                  "exchange_ns={} local_ns={} bid_price={:.12g} "
                  "bid_volume={:.12g} ask_price={:.12g} ask_volume={:.12g}\n"),
      last.id, last.symbol_id, magic_enum::enum_name(last.exchange),
      last.exchange_ns, last.local_ns, last.bid_price, last.bid_volume,
      last.ask_price, last.ask_volume);
}

template <typename TransportSocketT>
int RunProbe(ProbeConfig config) {
  g_stop_requested = 0;
  ProbeRunner<TransportSocketT> runner(std::move(config));
  runner.Start();

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(runner.config().duration_ms);
  while (std::chrono::steady_clock::now() < deadline && !runner.done()) {
    if (g_stop_requested != 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  runner.Stop();
  runner.Join();
  PrintSummary(runner);
  return runner.result() && runner.saw_active() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  InstallStopHandlers();

  ProbeConfig config{};
  bool no_tls = false;
  CLI::App app{"Binance USD-M futures bookTicker live probe"};
  app.add_option("--host", config.host, "remote host");
  app.add_option("--port", config.port, "remote port");
  app.add_option("--contract", config.contract, "Binance futures symbol");
  app.add_option("--symbol-id", config.symbol_id, "internal symbol id");
  app.add_option("--duration-ms", config.duration_ms, "sample duration");
  app.add_option("--cpu", config.cpu, "owner cpu id");
  app.add_flag("--tls", config.tls, "enable TLS");
  app.add_flag("--no-tls", no_tls, "disable TLS");
  CLI11_PARSE(app, argc, argv);
  if (no_tls) {
    config.tls = false;
  }

  if (config.tls) {
    return RunProbe<ws::TlsSocket>(std::move(config));
  }
  return RunProbe<ws::PlainSocket>(std::move(config));
}
