#include "core/websocket/handshake.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

using namespace aquila::websocket;

TEST(WebsocketHandshakeTest, BuildsClientHandshakeAndValidatesServerResponse) {
  constexpr std::string_view kClientKey = "dGhlIHNhbXBsZSBub25jZQ==";
  std::array<char, 512> output{};
  auto built = BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt", kClientKey,
                                    output);
  ASSERT_TRUE(built.ok);
  EXPECT_NE(built.bytes.find("Upgrade: websocket"), std::string_view::npos);

  constexpr std::string_view kValidResponse =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
      "\r\n";
  EXPECT_TRUE(ValidateServerHandshake(kValidResponse, kClientKey));

  constexpr std::string_view kInvalidStatusResponse =
      "HTTP/1.1 1010 Not Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
      "\r\n";
  EXPECT_FALSE(ValidateServerHandshake(kInvalidStatusResponse, kClientKey));

  constexpr std::string_view kInvalidAcceptResponse =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: invalid-accept-value=\r\n"
      "\r\n";
  EXPECT_FALSE(ValidateServerHandshake(kInvalidAcceptResponse, kClientKey));
}
