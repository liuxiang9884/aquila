#include "core/websocket/gate_ws_client.h"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <cstddef>
#include <string>

using namespace aquila::websocket;

namespace {

DeliveryResult CountPayload(void* context, const MessageView& view) noexcept {
  auto* bytes = static_cast<size_t*>(context);
  *bytes += view.payload.size();
  return DeliveryResult::kAccepted;
}

const char* ToString(ConnectionPhase phase) noexcept {
  switch (phase) {
    case ConnectionPhase::kDisconnected:
      return "disconnected";
    case ConnectionPhase::kResolving:
      return "resolving";
    case ConnectionPhase::kTcpConnecting:
      return "tcp_connecting";
    case ConnectionPhase::kTlsHandshaking:
      return "tls_handshaking";
    case ConnectionPhase::kWsHandshaking:
      return "ws_handshaking";
    case ConnectionPhase::kActive:
      return "active";
    case ConnectionPhase::kReconnectBackoff:
      return "reconnect_backoff";
    case ConnectionPhase::kClosing:
      return "closing";
    case ConnectionPhase::kClosed:
      return "closed";
  }
  return "unknown";
}

const char* ToString(ConnectionError error) noexcept {
  switch (error) {
    case ConnectionError::kNone:
      return "none";
    case ConnectionError::kResolveFailure:
      return "resolve_failure";
    case ConnectionError::kSocketError:
      return "socket_error";
    case ConnectionError::kConnectTimeout:
      return "connect_timeout";
    case ConnectionError::kTlsFailure:
      return "tls_failure";
    case ConnectionError::kHandshakeFailure:
      return "handshake_failure";
    case ConnectionError::kProtocolError:
      return "protocol_error";
    case ConnectionError::kHeartbeatTimeout:
      return "heartbeat_timeout";
    case ConnectionError::kPeerClosed:
      return "peer_closed";
  }
  return "unknown";
}

void PrintState(void*, ConnectionPhase phase) noexcept {
  std::fprintf(stderr, "state=%s\n", ToString(phase));
}

void PrintError(void*, ConnectionError error) noexcept {
  std::fprintf(stderr, "error=%s\n", ToString(error));
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"critical gate websocket probe"};
  std::string host{"fx-ws.gateio.ws"};
  std::string port{"443"};
  std::string target{"/v4/ws/usdt"};
  int cpu{-1};
  bool tls{true};
  bool no_tls{false};
  app.add_option("--host", host, "remote host");
  app.add_option("--port", port, "remote port");
  app.add_option("--target", target, "websocket target");
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

  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &CountPayload};
  GateWsClient client(config, consumer);
  client.SetStateHandler(nullptr, &PrintState);
  client.SetErrorHandler(nullptr, &PrintError);
  return client.Start() ? 0 : 1;
}
