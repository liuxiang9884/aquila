#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
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

#include "core/websocket/frame_codec.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/market_data/client.h"
#include "exchange/gate/market_data/subscription.h"
#include "exchange/gate/sbe/message_dispatcher.h"
#include <netdb.h>

namespace {

namespace gate = aquila::gate;
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
  std::uint64_t messages{0};
  std::uint64_t text_messages{0};
  std::uint64_t binary_messages{0};
  std::uint64_t non_final_messages{0};
  std::uint64_t sbe_ready_messages{0};
  std::uint64_t sbe_need_more_messages{0};
  std::uint64_t unsupported_schema_messages{0};
  std::uint64_t unsupported_schema_version_messages{0};
  std::uint64_t unknown_template_messages{0};
  std::uint64_t known_non_book_ticker_messages{0};
  std::uint64_t book_ticker_messages{0};
  std::uint64_t decoded_book_tickers{0};
  std::uint64_t failed_book_tickers{0};
  std::uint64_t processing_samples{0};
  std::uint64_t processing_total_ns{0};
  std::uint64_t processing_max_ns{0};
  std::uint64_t arrival_gap_samples{0};
  std::uint64_t arrival_gap_total_ns{0};
  std::uint64_t arrival_gap_max_ns{0};
  std::uint64_t first_arrival_ns{0};
  std::uint64_t last_arrival_ns{0};
  aquila::BookTicker first_book_ticker{};
  aquila::BookTicker last_book_ticker{};
  std::array<char, 512> last_text{};
  size_t last_text_size{0};
  bool last_text_truncated{false};
  bool has_first_book_ticker{false};
};

void RecordProcessingNs(ProbeStats* stats, std::uint64_t processing_ns) {
  ++stats->processing_samples;
  stats->processing_total_ns += processing_ns;
  stats->processing_max_ns = std::max(stats->processing_max_ns, processing_ns);
}

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

void SaveLastText(ProbeStats* stats, const ws::MessageView& view) noexcept {
  const size_t capacity = stats->last_text.size() - 1;
  const size_t copy_size = std::min(view.payload.size(), capacity);
  std::memcpy(stats->last_text.data(), view.payload.data(), copy_size);
  stats->last_text[copy_size] = '\0';
  stats->last_text_size = copy_size;
  stats->last_text_truncated = view.payload.size() > capacity;
}

void RecordDispatchStatus(ProbeStats* stats,
                          gate::SbeDispatchStatus status) noexcept {
  switch (status) {
    case gate::SbeDispatchStatus::kReady:
      ++stats->sbe_ready_messages;
      return;
    case gate::SbeDispatchStatus::kNeedMore:
      ++stats->sbe_need_more_messages;
      return;
    case gate::SbeDispatchStatus::kUnsupportedSchema:
      ++stats->unsupported_schema_messages;
      return;
    case gate::SbeDispatchStatus::kUnsupportedSchemaVersion:
      ++stats->unsupported_schema_version_messages;
      return;
    case gate::SbeDispatchStatus::kUnsupportedTemplate:
      ++stats->unknown_template_messages;
      return;
  }
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
    ++stats->decoded_book_tickers;
    stats->last_book_ticker = book_ticker;
  }
};

struct ProbeConfig {
  std::string host{"fx-ws.gateio.ws"};
  std::string port{"443"};
  std::string target{"/v4/ws/usdt/sbe?sbe_schema_id=1"};
  std::string contract{"BTC_USDT"};
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
        symbols_{gate::SymbolBinding{.symbol = config_.contract,
                                     .symbol_id = config_.symbol_id}},
        consumer_{.stats = &stats_},
        market_data_client_(symbols_, consumer_),
        encoder_(4096, 4096) {
    const std::array<std::string_view, 1> contracts{config_.contract};
    subscription_ = gate::BuildFuturesBookTickerSubscribeRequest(
        contracts, static_cast<std::int64_t>(std::time(nullptr)));
  }

  void Start() {
    ws::ConnectionConfig connection_config{};
    connection_config.host = config_.host;
    connection_config.service = config_.port;
    connection_config.target = config_.target;
    connection_config.enable_tls = TransportSocketT::kUsesTls;
    connection_config.max_reads_per_drive = 8;
    connection_config.read_until_would_block = false;
    connection_config.runtime_policy.io_cpu_id = config_.cpu;
    connection_config.runtime_policy.affinity_mode =
        config_.cpu >= 0 ? ws::AffinityMode::kBestEffort
                         : ws::AffinityMode::kNone;

    auto message_handler = ws::MakeMessageHandler(*this);
    client_ =
        std::make_unique<Client>(std::move(connection_config), message_handler);
    client_->SetStateHandler(this, &ProbeRunner::HandleState);
    client_->SetErrorHandler(this, &ProbeRunner::HandleError);

    thread_ = std::thread([this]() {
      result_.store(client_->Start(), std::memory_order_release);
      metrics_ = client_->SnapshotMetrics();
      done_.store(true, std::memory_order_release);
    });
  }

  void Stop() noexcept {
    if (client_ != nullptr) {
      client_->Stop();
    }
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

  [[nodiscard]] ws::SendStatus subscribe_status() const noexcept {
    return static_cast<ws::SendStatus>(
        subscribe_status_.load(std::memory_order_acquire));
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
  [[nodiscard]] std::string_view subscription() const noexcept {
    return subscription_;
  }

  ws::DeliveryResult Handle(const ws::MessageView& view) noexcept {
    return OnMessage(view);
  }

 private:
  using MessageHandler = ws::MessageHandlerRef<ProbeRunner<TransportSocketT>>;
  using Client = ws::BasicWebSocketClient<TransportSocketT, MessageHandler>;

  static void HandleState(void* context, ws::ConnectionPhase phase) noexcept {
    static_cast<ProbeRunner*>(context)->OnState(phase);
  }

  static void HandleError(void* context, ws::ConnectionError error) noexcept {
    static_cast<ProbeRunner*>(context)->OnError(error);
  }

  ws::DeliveryResult OnMessage(const ws::MessageView& view) noexcept {
    const std::uint64_t begin_ns = ws::NowNs(ws::ClockSource::kSteady);
    ++stats_.messages;
    RecordArrivalNs(&stats_, begin_ns);

    if (!view.fin) {
      ++stats_.non_final_messages;
    }

    ws::DeliveryResult result = ws::DeliveryResult::kAccepted;
    if (view.kind == ws::PayloadKind::kText) {
      ++stats_.text_messages;
      SaveLastText(&stats_, view);
    } else if (view.kind == ws::PayloadKind::kBinary) {
      ++stats_.binary_messages;
      result = OnBinaryMessage(view, begin_ns);
    }

    const std::uint64_t end_ns = ws::NowNs(ws::ClockSource::kSteady);
    RecordProcessingNs(&stats_, end_ns - begin_ns);
    return result;
  }

  ws::DeliveryResult OnBinaryMessage(const ws::MessageView& view,
                                     std::uint64_t arrival_ns) noexcept {
    if (!view.fin) {
      return ws::DeliveryResult::kAccepted;
    }

    const std::string_view payload{
        reinterpret_cast<const char*>(view.payload.data()),
        view.payload.size()};
    const gate::SbeDispatchResult dispatch = gate::DispatchSbeMessage(payload);
    RecordDispatchStatus(&stats_, dispatch.status);
    if (dispatch.status != gate::SbeDispatchStatus::kReady) {
      return ws::DeliveryResult::kAccepted;
    }

    if (dispatch.message_type != gate::GateSbeMessageType::kBookTicker) {
      ++stats_.known_non_book_ticker_messages;
      return ws::DeliveryResult::kAccepted;
    }

    ++stats_.book_ticker_messages;
    const std::uint64_t decoded_before = stats_.decoded_book_tickers;
    const ws::DeliveryResult result = market_data_client_.OnMessage(
        view, static_cast<std::int64_t>(arrival_ns));
    if (stats_.decoded_book_tickers == decoded_before) {
      ++stats_.failed_book_tickers;
    }
    return result;
  }

  void OnState(ws::ConnectionPhase phase) noexcept {
    phase_.store(static_cast<std::uint8_t>(phase), std::memory_order_release);
    fmt::print(stderr, FMT_COMPILE("state={}\n"), magic_enum::enum_name(phase));
    if (phase != ws::ConnectionPhase::kActive || client_ == nullptr) {
      return;
    }

    saw_active_.store(true, std::memory_order_release);
    PrintSocketInfo(client_->Core().NativeFd());
    bool expected = false;
    if (subscribed_.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel)) {
      const ws::SendStatus status = SubmitSubscription();
      subscribe_status_.store(static_cast<std::uint8_t>(status),
                              std::memory_order_release);
      fmt::print(stderr, FMT_COMPILE("subscribe={}\n"),
                 magic_enum::enum_name(status));
    }
  }

  void OnError(ws::ConnectionError error) noexcept {
    error_.store(static_cast<std::uint8_t>(error), std::memory_order_release);
    fmt::print(stderr, FMT_COMPILE("error={}\n"), magic_enum::enum_name(error));
  }

  ws::SendStatus SubmitSubscription() noexcept {
    if (client_ == nullptr) {
      return ws::SendStatus::kWriteUnavailable;
    }

    auto& core = client_->Core();
    ws::PreparedWrite* write = core.TryAcquirePreparedWrite();
    if (write == nullptr) {
      return ws::SendStatus::kNoPreparedWriteSlot;
    }

    const auto payload = std::as_bytes(
        std::span<const char>(subscription_.data(), subscription_.size()));
    const auto encoded = encoder_.EncodeText(payload, write->storage);
    if (!encoded.ok) {
      core.CancelPreparedWrite(write);
      return ws::SendStatus::kEncodeFailed;
    }

    write->encoded_size = static_cast<std::uint32_t>(encoded.bytes.size());
    write->write_offset = 0;
    write->kind = ws::PayloadKind::kText;
    const ws::SendStatus status = core.CommitPreparedWrite(write);
    if (status != ws::SendStatus::kOk) {
      core.CancelPreparedWrite(write);
    }
    return status;
  }

  ProbeConfig config_;
  std::array<gate::SymbolBinding, 1> symbols_;
  ProbeStats stats_{};
  ProbeConsumer consumer_;
  gate::FuturesMarketDataClient<ProbeConsumer> market_data_client_;
  std::string subscription_;
  ws::FrameCodec encoder_;
  std::unique_ptr<Client> client_;
  std::thread thread_;
  ws::Metrics metrics_{};
  std::atomic<bool> done_{false};
  std::atomic<bool> result_{false};
  std::atomic<bool> saw_active_{false};
  std::atomic<bool> subscribed_{false};
  std::atomic<std::uint8_t> phase_{
      static_cast<std::uint8_t>(ws::ConnectionPhase::kDisconnected)};
  std::atomic<std::uint8_t> error_{
      static_cast<std::uint8_t>(ws::ConnectionError::kNone)};
  std::atomic<std::uint8_t> subscribe_status_{
      static_cast<std::uint8_t>(ws::SendStatus::kWriteUnavailable)};
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
  const ws::Metrics metrics = runner.metrics();
  const ProbeConfig& config = runner.config();
  const aquila::BookTicker& first = stats.first_book_ticker;
  const aquila::BookTicker& last = stats.last_book_ticker;
  const std::int64_t book_ticker_id_delta =
      stats.has_first_book_ticker ? last.id - first.id : 0;

  fmt::print(FMT_COMPILE("config host={} port={} target={} tls={} contract={} "
                         "symbol_id={} duration_ms={} subscription={}\n"),
             config.host, config.port, config.target, config.tls ? "yes" : "no",
             config.contract, config.symbol_id, config.duration_ms,
             runner.subscription());
  fmt::print(
      FMT_COMPILE("result={} active={} phase={} error={} subscribe={} "
                  "rx_messages={} rx_bytes={} tx_messages={} tx_bytes={} "
                  "reconnects={} heartbeat_timeouts={}\n"),
      runner.result() ? "ok" : "failed", runner.saw_active() ? "yes" : "no",
      magic_enum::enum_name(runner.phase()),
      magic_enum::enum_name(runner.error()),
      magic_enum::enum_name(runner.subscribe_status()), metrics.rx_messages,
      metrics.rx_bytes, metrics.tx_messages, metrics.tx_bytes,
      metrics.reconnects, metrics.heartbeat_timeouts);
  fmt::print(FMT_COMPILE("messages total={} text={} binary={} non_final={} "
                         "sbe_ready={} sbe_need_more={} unsupported_schema={} "
                         "unsupported_schema_version={} unknown_template={} "
                         "known_non_book_ticker={}\n"),
             stats.messages, stats.text_messages, stats.binary_messages,
             stats.non_final_messages, stats.sbe_ready_messages,
             stats.sbe_need_more_messages, stats.unsupported_schema_messages,
             stats.unsupported_schema_version_messages,
             stats.unknown_template_messages,
             stats.known_non_book_ticker_messages);
  fmt::print(
      FMT_COMPILE("book_ticker payloads={} decoded={} failed_or_unmapped={} "
                  "first_id={} last_id={} id_delta={}\n"),
      stats.book_ticker_messages, stats.decoded_book_tickers,
      stats.failed_book_tickers, first.id, last.id, book_ticker_id_delta);
  fmt::print(FMT_COMPILE("processing_ns samples={} avg={:.2f} max={}\n"),
             stats.processing_samples,
             AverageNs(stats.processing_total_ns, stats.processing_samples),
             stats.processing_max_ns);
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
  if (stats.last_text_size != 0) {
    fmt::print(FMT_COMPILE("last_text={}{}\n"), stats.last_text.data(),
               stats.last_text_truncated ? "...(truncated)" : "");
  }
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
  CLI::App app{"Gate futures SBE book_ticker live probe"};
  app.add_option("--host", config.host, "remote host");
  app.add_option("--port", config.port, "remote port");
  app.add_option("--target", config.target, "websocket target");
  app.add_option("--contract", config.contract, "Gate futures contract");
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
