#include "core/websocket/websocket_client.h"

#include <CLI/CLI.hpp>
#include <fmt/compile.h>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

using namespace aquila::websocket;

namespace {

struct ProbeContext {
  size_t bytes{0};
  ConnectionPhase phase{ConnectionPhase::kDisconnected};
  ConnectionError error{ConnectionError::kNone};
};

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

void RecordState(void* context, ConnectionPhase phase) noexcept {
  auto* probe = static_cast<ProbeContext*>(context);
  probe->phase = phase;
  PrintState(nullptr, phase);
}

void RecordError(void* context, ConnectionError error) noexcept {
  auto* probe = static_cast<ProbeContext*>(context);
  probe->error = error;
  PrintError(nullptr, error);
}

}  // namespace

template <typename ClientT>
int RunProbe(ConnectionConfig config) {
  ProbeContext probe{};
  MessageConsumer consumer{&probe, &CountPayload};
  ClientT client(std::move(config), consumer);
  client.SetStateHandler(&probe, &RecordState);
  client.SetErrorHandler(&probe, &RecordError);
  const bool ok = client.Start();
  const Metrics metrics = client.SnapshotMetrics();
  const std::string_view final_state = magic_enum::enum_name(probe.phase);
  const std::string_view final_error = magic_enum::enum_name(probe.error);
  fmt::print(stderr,
             FMT_COMPILE("result={} final_state={} final_error={} rx_bytes={} "
                         "tx_bytes={} rx_messages={} tx_messages={} "
                         "heartbeat_timeouts={}\n"),
             ok ? "ok" : "failed", final_state, final_error, probe.bytes,
             metrics.tx_bytes, metrics.rx_messages, metrics.tx_messages,
             metrics.heartbeat_timeouts);
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  CLI::App app{"critical websocket probe"};
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

  if (tls) {
    return RunProbe<WebSocketClient>(std::move(config));
  }
  return RunProbe<PlainWebSocketClient>(std::move(config));
}
