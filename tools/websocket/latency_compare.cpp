#include <pthread.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/compile.h>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include "core/websocket/frame_codec.h"
#include "core/websocket/websocket_client.h"
#include "tools/websocket/latency_compare_support.h"
#include <netdb.h>
#include <sched.h>

namespace {

using aquila::tools::BuildGateSubscribeRequest;
using aquila::tools::EndpointSide;
using aquila::tools::LatencyPairCollector;
using aquila::tools::SelectWarmupPrimary;
using aquila::tools::TryParseGateBookTicker;
using aquila::tools::WarmupPrimary;
using aquila::tools::WarmupSelectionInput;
namespace ws = aquila::websocket;

std::uint64_t NowNs() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

struct EndpointConfig {
  std::string label;
  std::string host;
  std::string port;
  std::string target;
  int cpu{-1};
};

struct EndpointCounters {
  std::uint64_t messages{0};
  std::uint64_t matched_updates{0};
  std::uint64_t ignored_messages{0};
};

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

int PrintSocketInfo(std::string_view label, int fd) {
  if (fd < 0) {
    fmt::print(stderr, FMT_COMPILE("{} socket fd=unavailable\n"), label);
    return -1;
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

  fmt::print(
      stderr,
      FMT_COMPILE("{} socket fd={} local={} peer={} so_incoming_cpu={}\n"),
      label, fd, has_local ? FormatSockaddr(local, local_len) : "unavailable",
      has_peer ? FormatSockaddr(peer, peer_len) : "unavailable",
      has_incoming_cpu ? fmt::format(FMT_COMPILE("{}"), incoming_cpu)
                       : "unavailable");
  return has_incoming_cpu ? incoming_cpu : -1;
}

bool PinCurrentThreadToCpu(int cpu) noexcept {
  if (cpu < 0) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

template <typename ClientT>
class EndpointRunner {
 public:
  EndpointRunner(EndpointConfig endpoint, EndpointSide side,
                 std::string subscription, LatencyPairCollector& collector,
                 bool pin_to_incoming_cpu)
      : endpoint_(std::move(endpoint)),
        side_(side),
        subscription_(std::move(subscription)),
        collector_(collector),
        pin_to_incoming_cpu_(pin_to_incoming_cpu),
        encoder_(4096, 4096) {}

  void Start() {
    ws::ConnectionConfig config{};
    config.host = endpoint_.host;
    config.service = endpoint_.port;
    config.target = endpoint_.target;
    config.enable_tls = ClientT::TransportUsesTls;
    config.max_reads_per_drive = 8;
    config.read_until_would_block = false;
    config.runtime_policy.io_cpu_id = endpoint_.cpu;
    config.runtime_policy.affinity_mode = endpoint_.cpu >= 0
                                              ? ws::AffinityMode::kBestEffort
                                              : ws::AffinityMode::kNone;

    ws::MessageCallback consumer{this, &EndpointRunner::HandleMessage};
    client_ = std::make_unique<ClientT>(std::move(config), consumer);
    client_->SetStateHandler(this, &EndpointRunner::HandleState);
    client_->SetErrorHandler(this, &EndpointRunner::HandleError);
    thread_ = std::thread([this]() {
      result_.store(client_->Start(), std::memory_order_release);
      metrics_ = client_->SnapshotMetrics();
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

  [[nodiscard]] EndpointCounters counters() const noexcept {
    return EndpointCounters{
        .messages = messages_.load(std::memory_order_acquire),
        .matched_updates = matched_updates_.load(std::memory_order_acquire),
        .ignored_messages = ignored_messages_.load(std::memory_order_acquire),
    };
  }

  [[nodiscard]] ws::Metrics metrics() const noexcept {
    return metrics_;
  }
  [[nodiscard]] std::string_view label() const noexcept {
    return endpoint_.label;
  }

 private:
  static ws::DeliveryResult HandleMessage(
      void* context, const ws::MessageView& view) noexcept {
    return static_cast<EndpointRunner*>(context)->OnMessage(view);
  }

  static void HandleState(void* context, ws::ConnectionPhase phase) noexcept {
    static_cast<EndpointRunner*>(context)->OnState(phase);
  }

  static void HandleError(void* context, ws::ConnectionError error) noexcept {
    static_cast<EndpointRunner*>(context)->OnError(error);
  }

  ws::DeliveryResult OnMessage(const ws::MessageView& view) noexcept {
    messages_.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t arrival_ns = NowNs();
    const auto payload =
        std::string_view(reinterpret_cast<const char*>(view.payload.data()),
                         view.payload.size());
    const auto parsed = TryParseGateBookTicker(payload);
    if (!parsed.has_value()) {
      ignored_messages_.fetch_add(1, std::memory_order_relaxed);
      return ws::DeliveryResult::kAccepted;
    }

    collector_.Observe(side_, *parsed, arrival_ns);
    matched_updates_.fetch_add(1, std::memory_order_relaxed);
    return ws::DeliveryResult::kAccepted;
  }

  void OnState(ws::ConnectionPhase phase) noexcept {
    phase_.store(static_cast<std::uint8_t>(phase), std::memory_order_release);
    const std::string_view phase_name = magic_enum::enum_name(phase);
    fmt::print(stderr, FMT_COMPILE("{} state={}\n"), endpoint_.label,
               phase_name);
    if (phase != ws::ConnectionPhase::kActive) {
      return;
    }
    saw_active_.store(true, std::memory_order_release);
    if (client_ != nullptr) {
      const int incoming_cpu =
          PrintSocketInfo(endpoint_.label, client_->Core().NativeFd());
      if (pin_to_incoming_cpu_) {
        const bool pinned = PinCurrentThreadToCpu(incoming_cpu);
        fmt::print(stderr, FMT_COMPILE("{} pin_to_incoming_cpu={} cpu={}\n"),
                   endpoint_.label, pinned ? "ok" : "failed", incoming_cpu);
      }
    }
    bool expected = false;
    if (subscribed_.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel)) {
      subscribe_status_.store(static_cast<std::uint8_t>(SubmitSubscription()),
                              std::memory_order_release);
    }
  }

  void OnError(ws::ConnectionError error) noexcept {
    error_.store(static_cast<std::uint8_t>(error), std::memory_order_release);
    const std::string_view error_name = magic_enum::enum_name(error);
    fmt::print(stderr, FMT_COMPILE("{} error={}\n"), endpoint_.label,
               error_name);
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

  EndpointConfig endpoint_;
  EndpointSide side_;
  std::string subscription_;
  LatencyPairCollector& collector_;
  bool pin_to_incoming_cpu_{false};
  ws::FrameCodec encoder_;
  std::unique_ptr<ClientT> client_;
  std::thread thread_;
  ws::Metrics metrics_{};
  std::atomic<bool> result_{false};
  std::atomic<bool> saw_active_{false};
  std::atomic<bool> subscribed_{false};
  std::atomic<std::uint8_t> phase_{
      static_cast<std::uint8_t>(ws::ConnectionPhase::kDisconnected)};
  std::atomic<std::uint8_t> error_{
      static_cast<std::uint8_t>(ws::ConnectionError::kNone)};
  std::atomic<std::uint8_t> subscribe_status_{
      static_cast<std::uint8_t>(ws::SendStatus::kWriteUnavailable)};
  std::atomic<std::uint64_t> messages_{0};
  std::atomic<std::uint64_t> matched_updates_{0};
  std::atomic<std::uint64_t> ignored_messages_{0};
};

std::int64_t Percentile(const std::vector<std::int64_t>& sorted,
                        long double percentile) {
  if (sorted.empty()) {
    return 0;
  }
  const long double rank =
      percentile * static_cast<long double>(sorted.size() - 1);
  const auto index = static_cast<size_t>(rank + 0.5L);
  return sorted[std::min(index, sorted.size() - 1)];
}

std::string_view WarmupPrimaryName(WarmupPrimary primary) noexcept {
  switch (primary) {
    case WarmupPrimary::kPublic:
      return "public";
    case WarmupPrimary::kPrivate:
      return "private";
    case WarmupPrimary::kNone:
      return "none";
  }
  return "none";
}

template <typename ClientT>
void PrintEndpointSummary(const EndpointRunner<ClientT>& runner) {
  const auto counters = runner.counters();
  const auto metrics = runner.metrics();
  fmt::print(
      FMT_COMPILE("{} result={} active={} phase={} error={} subscribe={} "
                  "messages={} parsed_updates={} ignored={} rx_messages={} "
                  "rx_bytes={} tx_messages={} tx_bytes={}\n"),
      runner.label(), runner.result() ? "ok" : "failed",
      runner.saw_active() ? "yes" : "no", magic_enum::enum_name(runner.phase()),
      magic_enum::enum_name(runner.error()),
      magic_enum::enum_name(runner.subscribe_status()), counters.messages,
      counters.matched_updates, counters.ignored_messages, metrics.rx_messages,
      metrics.rx_bytes, metrics.tx_messages, metrics.tx_bytes);
}

template <typename PublicClientT, typename PrivateClientT>
int RunCompare(const std::string& public_host, const std::string& public_port,
               const std::string& public_target, int public_cpu,
               const std::string& private_host, const std::string& private_port,
               const std::string& private_target, int private_cpu,
               const std::string& channel, const std::string& contract,
               std::uint32_t duration_ms, size_t max_pending,
               bool pin_to_incoming_cpu) {
  const auto epoch_seconds = static_cast<std::uint64_t>(std::time(nullptr));
  const std::string subscription =
      BuildGateSubscribeRequest(channel, contract, epoch_seconds);

  LatencyPairCollector collector(max_pending);
  EndpointRunner<PublicClientT> public_runner(
      EndpointConfig{
          .label = "public",
          .host = public_host,
          .port = public_port,
          .target = public_target,
          .cpu = public_cpu,
      },
      EndpointSide::kPublic, subscription, collector, pin_to_incoming_cpu);
  EndpointRunner<PrivateClientT> private_runner(
      EndpointConfig{
          .label = "private",
          .host = private_host,
          .port = private_port,
          .target = private_target,
          .cpu = private_cpu,
      },
      EndpointSide::kPrivate, subscription, collector, pin_to_incoming_cpu);

  fmt::print(FMT_COMPILE("subscription={}\n"), subscription);
  public_runner.Start();
  private_runner.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
  public_runner.Stop();
  private_runner.Stop();
  public_runner.Join();
  private_runner.Join();

  PrintEndpointSummary(public_runner);
  PrintEndpointSummary(private_runner);

  const auto snapshot = collector.Snapshot();
  std::vector<std::int64_t> leads;
  leads.reserve(snapshot.matched.size());
  size_t private_faster = 0;
  size_t public_faster = 0;
  size_t ties = 0;
  for (const auto& sample : snapshot.matched) {
    leads.push_back(sample.private_lead_ns);
    if (sample.private_lead_ns > 0) {
      ++private_faster;
    } else if (sample.private_lead_ns < 0) {
      ++public_faster;
    } else {
      ++ties;
    }
  }
  std::sort(leads.begin(), leads.end());
  const std::int64_t private_lead_p50 = Percentile(leads, 0.50L);
  const std::int64_t private_lead_p99 = Percentile(leads, 0.99L);

  fmt::print(
      FMT_COMPILE("matched={} private_faster={} public_faster={} ties={} "
                  "pending_public={} pending_private={}\n"),
      snapshot.matched.size(), private_faster, public_faster, ties,
      snapshot.pending_public, snapshot.pending_private);
  fmt::print(
      FMT_COMPILE("private_lead_ns min={} p50={} p99={} p99.9={} max={}\n"),
      leads.empty() ? 0 : leads.front(), private_lead_p50, private_lead_p99,
      Percentile(leads, 0.999L), leads.empty() ? 0 : leads.back());

  const auto selection = SelectWarmupPrimary(WarmupSelectionInput{
      .public_healthy = public_runner.result() && public_runner.saw_active() &&
                        public_runner.subscribe_status() == ws::SendStatus::kOk,
      .private_healthy =
          private_runner.result() && private_runner.saw_active() &&
          private_runner.subscribe_status() == ws::SendStatus::kOk,
      .matched = snapshot.matched.size(),
      .private_faster = private_faster,
      .public_faster = public_faster,
      .ties = ties,
      .pending_public = snapshot.pending_public,
      .pending_private = snapshot.pending_private,
      .private_lead_p50_ns = private_lead_p50,
      .private_lead_p99_ns = private_lead_p99,
  });
  fmt::print(FMT_COMPILE("selected={} reason={}\n"),
             WarmupPrimaryName(selection.selected), selection.reason);

  if (!public_runner.saw_active() || !private_runner.saw_active()) {
    return 2;
  }
  return snapshot.matched.empty() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"compare Gate public/private WebSocket market-data latency"};
  std::string public_host{"fx-ws.gateio.ws"};
  std::string private_host{"fxws-private.gateapi.io"};
  std::string port{"443"};
  std::string public_port{"443"};
  std::string private_port{"443"};
  std::string public_target{"/v4/ws/usdt"};
  std::string private_target{"/v4/ws/usdt"};
  std::string channel{"futures.book_ticker"};
  std::string contract{"BTC_USDT"};
  int public_cpu{-1};
  int private_cpu{-1};
  std::uint32_t duration_ms{60'000};
  size_t max_pending{65'536};
  bool public_tls{true};
  bool public_no_tls{false};
  bool private_tls{true};
  bool private_no_tls{false};
  bool pin_to_incoming_cpu{false};

  app.add_option("--public-host", public_host, "public WebSocket host");
  app.add_option("--private-host", private_host, "private WebSocket host");
  auto* common_port = app.add_option("--port", port, "remote port");
  auto* public_port_option =
      app.add_option("--public-port", public_port, "public remote port");
  auto* private_port_option =
      app.add_option("--private-port", private_port, "private remote port");
  app.add_option("--public-target", public_target, "public WebSocket target");
  app.add_option("--private-target", private_target,
                 "private WebSocket target");
  app.add_option("--channel", channel, "Gate WebSocket channel");
  app.add_option("--contract", contract, "Gate contract");
  app.add_option("--public-cpu", public_cpu, "public connection owner CPU");
  app.add_option("--private-cpu", private_cpu, "private connection owner CPU");
  app.add_option("--duration-ms", duration_ms, "sample duration");
  app.add_option("--max-pending", max_pending, "maximum unmatched update keys");
  app.add_flag("--public-tls", public_tls, "enable TLS for public endpoint");
  app.add_flag("--public-no-tls", public_no_tls,
               "disable TLS for public endpoint");
  app.add_flag("--private-tls", private_tls, "enable TLS for private endpoint");
  app.add_flag("--private-no-tls", private_no_tls,
               "disable TLS for private endpoint");
  app.add_flag("--pin-to-incoming-cpu", pin_to_incoming_cpu,
               "pin each owner thread to SO_INCOMING_CPU after activation");
  CLI11_PARSE(app, argc, argv);

  if (common_port->count() != 0 && public_port_option->count() == 0) {
    public_port = port;
  }
  if (common_port->count() != 0 && private_port_option->count() == 0) {
    private_port = port;
  }
  if (public_no_tls) {
    public_tls = false;
  }
  if (private_no_tls) {
    private_tls = false;
  }

  if (public_tls && private_tls) {
    return RunCompare<ws::WebSocketClient, ws::WebSocketClient>(
        public_host, public_port, public_target, public_cpu, private_host,
        private_port, private_target, private_cpu, channel, contract,
        duration_ms, max_pending, pin_to_incoming_cpu);
  }
  if (public_tls && !private_tls) {
    return RunCompare<ws::WebSocketClient, ws::PlainWebSocketClient>(
        public_host, public_port, public_target, public_cpu, private_host,
        private_port, private_target, private_cpu, channel, contract,
        duration_ms, max_pending, pin_to_incoming_cpu);
  }
  if (!public_tls && private_tls) {
    return RunCompare<ws::PlainWebSocketClient, ws::WebSocketClient>(
        public_host, public_port, public_target, public_cpu, private_host,
        private_port, private_target, private_cpu, channel, contract,
        duration_ms, max_pending, pin_to_incoming_cpu);
  }
  return RunCompare<ws::PlainWebSocketClient, ws::PlainWebSocketClient>(
      public_host, public_port, public_target, public_cpu, private_host,
      private_port, private_target, private_cpu, channel, contract, duration_ms,
      max_pending, pin_to_incoming_cpu);
}
