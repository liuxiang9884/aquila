#include "core/websocket/websocket_client.h"

#include <gtest/gtest.h>

using namespace aquila::websocket;

namespace {

DeliveryResult AcceptAll(void*, const MessageView&) noexcept {
  return DeliveryResult::kAccepted;
}

}  // namespace

TEST(WebSocketLoopbackIntegrationTest, PreparesRuntimeForLoopbackConfig) {
  ConnectionConfig config{};
  config.host = "127.0.0.1";
  config.service = "9443";
  config.target = "/v4/ws/usdt";
  MessageConsumer consumer{nullptr, &AcceptAll};
  WebSocketClient client(config, consumer);
  EXPECT_TRUE(client.PrepareRuntimeOnly());
}
