#include "core/websocket/frame_codec.h"
#include "core/websocket/websocket_client.h"

#include <CLI/CLI.hpp>
#include <fmt/compile.h>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include <cstddef>
#include <cstdio>
#include <netdb.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>

using namespace aquila::websocket;

namespace {

struct ProbeContext {
  size_t bytes{0};
  ConnectionPhase phase{ConnectionPhase::kDisconnected};
  ConnectionError error{ConnectionError::kNone};
};

std::string FormatSockaddr(const sockaddr_storage& storage,
                           socklen_t storage_len) {
  char host[NI_MAXHOST]{};
  char service[NI_MAXSERV]{};
  const int rc =
      ::getnameinfo(reinterpret_cast<const sockaddr*>(&storage), storage_len,
                    host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV);
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
             FMT_COMPILE(
                 "socket fd={} local={} peer={} so_incoming_cpu={}\n"),
             fd, has_local ? FormatSockaddr(local, local_len) : "unavailable",
             has_peer ? FormatSockaddr(peer, peer_len) : "unavailable",
             has_incoming_cpu ? fmt::format(FMT_COMPILE("{}"), incoming_cpu)
                              : "unavailable");
}

DeliveryResult CountPayload(void* context, const MessageView& view) noexcept {
  auto* probe = static_cast<ProbeContext*>(context);
  probe->bytes += view.payload.size();
  return DeliveryResult::kAccepted;
}

void PrintState(void*, ConnectionPhase phase) noexcept {
  const std::string_view phase_name = magic_enum::enum_name(phase);
  fmt::print(stderr, FMT_COMPILE("state={}\n"), phase_name);
}

void PrintError(void*, ConnectionError error) noexcept {
  const std::string_view error_name = magic_enum::enum_name(error);
  fmt::print(stderr, FMT_COMPILE("error={}\n"), error_name);
}

template <typename ClientT>
struct ProbeRuntime {
  explicit ProbeRuntime(std::string subscribe_text) noexcept
      : subscribe(std::move(subscribe_text)), encoder(4096, 4096) {}

  ProbeContext probe{};
  ClientT* client{nullptr};
  std::string subscribe;
  FrameCodec encoder;
  bool subscribed{false};
  SendStatus subscribe_status{SendStatus::kWriteUnavailable};
};

template <typename ClientT>
SendStatus SubmitSubscription(ProbeRuntime<ClientT>& runtime) noexcept {
  if (runtime.client == nullptr || runtime.subscribe.empty()) {
    return SendStatus::kWriteUnavailable;
  }

  auto& core = runtime.client->Core();
  PreparedWrite* write = core.TryAcquirePreparedWrite();
  if (write == nullptr) {
    return SendStatus::kNoPreparedWriteSlot;
  }

  const auto payload = std::as_bytes(
      std::span<const char>(runtime.subscribe.data(), runtime.subscribe.size()));
  const auto encoded = runtime.encoder.EncodeText(payload, write->storage);
  if (!encoded.ok) {
    core.CancelPreparedWrite(write);
    return SendStatus::kEncodeFailed;
  }

  write->encoded_size = static_cast<std::uint32_t>(encoded.bytes.size());
  write->write_offset = 0;
  write->kind = PayloadKind::kText;
  const SendStatus status = core.CommitPreparedWrite(write);
  if (status != SendStatus::kOk) {
    core.CancelPreparedWrite(write);
  }
  return status;
}

template <typename ClientT>
void RecordStateAndMaybeSubscribe(void* context,
                                  ConnectionPhase phase) noexcept {
  auto* runtime = static_cast<ProbeRuntime<ClientT>*>(context);
  runtime->probe.phase = phase;
  PrintState(nullptr, phase);
  if (phase != ConnectionPhase::kActive || runtime->client == nullptr) {
    return;
  }

  PrintSocketInfo(runtime->client->Core().NativeFd());
  if (!runtime->subscribe.empty() && !runtime->subscribed) {
    runtime->subscribed = true;
    runtime->subscribe_status = SubmitSubscription(*runtime);
    fmt::print(stderr, FMT_COMPILE("subscribe={}\n"),
               magic_enum::enum_name(runtime->subscribe_status));
  }
}

template <typename ClientT>
void RecordRuntimeError(void* context, ConnectionError error) noexcept {
  auto* runtime = static_cast<ProbeRuntime<ClientT>*>(context);
  runtime->probe.error = error;
  PrintError(nullptr, error);
}

}  // namespace

template <typename ClientT>
int RunProbe(ConnectionConfig config, std::string subscribe) {
  ProbeRuntime<ClientT> runtime(std::move(subscribe));
  MessageConsumer consumer{&runtime.probe, &CountPayload};
  ClientT client(std::move(config), consumer);
  runtime.client = &client;
  client.SetStateHandler(&runtime, &RecordStateAndMaybeSubscribe<ClientT>);
  client.SetErrorHandler(&runtime, &RecordRuntimeError<ClientT>);
  const bool ok = client.Start();
  const Metrics metrics = client.SnapshotMetrics();
  const std::string_view final_state = magic_enum::enum_name(runtime.probe.phase);
  const std::string_view final_error = magic_enum::enum_name(runtime.probe.error);
  fmt::print(stderr,
             FMT_COMPILE("result={} final_state={} final_error={} rx_bytes={} "
                         "tx_bytes={} rx_messages={} tx_messages={} "
                         "heartbeat_timeouts={} subscribe={}\n"),
             ok ? "ok" : "failed", final_state, final_error,
             runtime.probe.bytes, metrics.tx_bytes, metrics.rx_messages,
             metrics.tx_messages, metrics.heartbeat_timeouts,
             magic_enum::enum_name(runtime.subscribe_status));
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  CLI::App app{"critical websocket probe"};
  std::string host{"fx-ws.gateio.ws"};
  std::string port{"443"};
  std::string target{"/v4/ws/usdt"};
  std::string subscribe{};
  int cpu{-1};
  bool tls{true};
  bool no_tls{false};
  app.add_option("--host", host, "remote host");
  app.add_option("--port", port, "remote port");
  app.add_option("--target", target, "websocket target");
  app.add_option("--subscribe", subscribe, "optional text frame to send once active");
  app.add_option("--cpu", cpu, "owner cpu id");
  app.add_flag("--tls", tls, "enable tls");
  app.add_flag("--no-tls", no_tls, "disable tls");
  CLI11_PARSE(app, argc, argv);
  if (no_tls) {
    tls = false;
  }

  ConnectionConfig config{};
  config.host = host;
  config.service = port;
  config.target = target;
  config.enable_tls = tls;
  config.runtime_policy.io_cpu_id = cpu;
  config.runtime_policy.affinity_mode =
      cpu >= 0 ? AffinityMode::kBestEffort : AffinityMode::kNone;

  if (tls) {
    return RunProbe<WebSocketClient>(std::move(config), std::move(subscribe));
  }
  return RunProbe<PlainWebSocketClient>(std::move(config), std::move(subscribe));
}
