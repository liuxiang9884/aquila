#include "core/websocket/gate_ws_client.h"

using namespace aquila::websocket;

namespace {

DeliveryResult AcceptAll(void*, const MessageView&) noexcept {
  return DeliveryResult::kAccepted;
}

}  // namespace

int main() {
  ConnectionConfig config{};
  config.host = "127.0.0.1";
  config.service = "9443";
  config.target = "/v4/ws/usdt";
  MessageConsumer consumer{nullptr, &AcceptAll};
  GateWsClient client(config, consumer);
  return client.PrepareRuntimeOnly() ? 0 : 1;
}
