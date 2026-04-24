#include "core/websocket/websocket_client.h"

#include <CLI/CLI.hpp>
#include <magic_enum/magic_enum.hpp>

#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

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
  std::fprintf(stderr, "state=%.*s\n", static_cast<int>(phase_name.size()),
               phase_name.data());
}

void PrintError(void*, ConnectionError error) noexcept {
  const std::string_view error_name = magic_enum::enum_name(error);
  std::fprintf(stderr, "error=%.*s\n", static_cast<int>(error_name.size()),
               error_name.data());
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

  ProbeContext probe{};
  MessageConsumer consumer{&probe, &CountPayload};
  WebSocketClient client(config, consumer);
  client.SetStateHandler(&probe, &RecordState);
  client.SetErrorHandler(&probe, &RecordError);
  const bool ok = client.Start();
  const Metrics metrics = client.SnapshotMetrics();
  const std::string_view final_state = magic_enum::enum_name(probe.phase);
  const std::string_view final_error = magic_enum::enum_name(probe.error);
  std::fprintf(stderr,
               "result=%s final_state=%.*s final_error=%.*s rx_bytes=%zu "
               "tx_bytes=%" PRIu64 " rx_messages=%" PRIu64
               " tx_messages=%" PRIu64 " heartbeat_timeouts=%" PRIu64 "\n",
               ok ? "ok" : "failed", static_cast<int>(final_state.size()),
               final_state.data(), static_cast<int>(final_error.size()),
               final_error.data(),
               probe.bytes, metrics.tx_bytes, metrics.rx_messages,
               metrics.tx_messages, metrics.heartbeat_timeouts);
  return ok ? 0 : 1;
}
