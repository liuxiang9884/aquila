#include "core/websocket/handshake.h"

#include <array>
#include <string_view>

using namespace aquila::websocket;

int main() {
  constexpr std::string_view kClientKey = "dGhlIHNhbXBsZSBub25jZQ==";
  std::array<char, 512> output{};
  auto built = BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt", kClientKey,
                                    output);
  if (!built.ok) {
    return 1;
  }
  if (built.bytes.find("Upgrade: websocket") == std::string_view::npos) {
    return 1;
  }

  constexpr std::string_view kValidResponse =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
      "\r\n";
  if (!ValidateServerHandshake(kValidResponse, kClientKey)) {
    return 1;
  }

  constexpr std::string_view kInvalidStatusResponse =
      "HTTP/1.1 1010 Not Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
      "\r\n";
  if (ValidateServerHandshake(kInvalidStatusResponse, kClientKey)) {
    return 1;
  }

  constexpr std::string_view kInvalidAcceptResponse =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: invalid-accept-value=\r\n"
      "\r\n";
  return ValidateServerHandshake(kInvalidAcceptResponse, kClientKey) ? 1 : 0;
}
