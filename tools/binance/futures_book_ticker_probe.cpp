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
#include "exchange/binance/market_data/data_session.h"

namespace {

namespace binance = aquila::binance;
namespace ws = aquila::websocket;

std::atomic<bool> g_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void RequestStop(int) noexcept {
  g_stop_requested.store(true, std::memory_order_relaxed);
}

void InstallStopHandlers() noexcept {
  std::signal(SIGINT, &RequestStop);
  std::signal(SIGTERM, &RequestStop);
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

template <typename WebSocketPolicy>
class ProbeRunner {
 public:
  explicit ProbeRunner(ProbeConfig config)
      : config_(std::move(config)),
        symbols_{binance::SymbolBinding{.symbol = config_.contract,
                                        .symbol_id = config_.symbol_id}},
        consumer_{.stats = &stats_},
        session_(BuildConnectionConfig(config_), symbols_, consumer_) {}

  void Start() {
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
    return session_.ever_active();
  }

  [[nodiscard]] ws::ConnectionPhase phase() const noexcept {
    return session_.phase();
  }

  [[nodiscard]] ws::ConnectionError error() const noexcept {
    return session_.last_error();
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

  [[nodiscard]] const binance::DataSessionStats& session_stats()
      const noexcept {
    return session_.stats();
  }

  [[nodiscard]] const binance::FuturesMarketDataClientStats& market_data_stats()
      const noexcept {
    return session_.market_data_client_diagnostics().stats();
  }

 private:
  using TransportSocket = typename WebSocketPolicy::TransportSocket;
  using Session = binance::DataSession<ProbeConsumer, WebSocketPolicy,
                                       binance::DataSessionDiagnosticsPolicy>;

  static ws::ConnectionConfig BuildConnectionConfig(const ProbeConfig& config) {
    ws::ConnectionConfig connection_config{};
    connection_config.host = config.host;
    connection_config.service = config.port;
    connection_config.enable_tls = TransportSocket::kUsesTls;
    connection_config.max_reads_per_drive = 8;
    connection_config.read_until_would_block = false;
    connection_config.runtime_policy.io_cpu_id = config.cpu;
    connection_config.runtime_policy.affinity_mode =
        config.cpu >= 0 ? ws::AffinityMode::kBestEffort
                        : ws::AffinityMode::kNone;
    return connection_config;
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
  const binance::DataSessionStats& session_stats = runner.session_stats();
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
  fmt::print(FMT_COMPILE("session text={} binary={} control={} "
                         "book_ticker={}\n"),
             session_stats.text_messages, session_stats.binary_messages,
             session_stats.control_messages,
             session_stats.book_ticker_messages);
  fmt::print(FMT_COMPILE("market_data malformed_json={} "
                         "book_ticker={} simdjson_padding_fallback={}\n"),
             market_data_stats.malformed_json_messages,
             market_data_stats.book_ticker_messages,
             market_data_stats.simdjson_padding_fallback_messages);
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

template <typename WebSocketPolicy>
int RunProbe(ProbeConfig config) {
  g_stop_requested.store(false, std::memory_order_relaxed);
  ProbeRunner<WebSocketPolicy> runner(std::move(config));
  runner.Start();

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(runner.config().duration_ms);
  while (std::chrono::steady_clock::now() < deadline && !runner.done()) {
    if (g_stop_requested.load(std::memory_order_relaxed)) {
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
    return RunProbe<aquila::binance::DefaultTlsWebSocketPolicy>(
        std::move(config));
  }
  return RunProbe<aquila::binance::DefaultPlainWebSocketPolicy>(
      std::move(config));
}
