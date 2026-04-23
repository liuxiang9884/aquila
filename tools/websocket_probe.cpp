#include "core/websocket/gate_ws_client.h"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <string>

using namespace aquila::websocket;

namespace {

DeliveryResult CountPayload(void* context, const MessageView& view) noexcept {
  auto* bytes = static_cast<size_t*>(context);
  *bytes += view.payload.size();
  return DeliveryResult::kAccepted;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"critical gate websocket probe"};
  std::string host{"fx-ws.gateio.ws"};
  std::string port{"443"};
  std::string target{"/v4/ws/usdt"};
  int cpu{-1};
  bool tls{false};
  app.add_option("--host", host, "remote host");
  app.add_option("--port", port, "remote port");
  app.add_option("--target", target, "websocket target");
  app.add_option("--cpu", cpu, "owner cpu id");
  app.add_flag("--tls", tls, "enable tls");
  CLI11_PARSE(app, argc, argv);

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
  return client.Start() ? 0 : 1;
}
