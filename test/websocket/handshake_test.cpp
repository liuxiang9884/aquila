#include "core/websocket/handshake.h"

#include <array>

using namespace aquila::websocket;

int main() {
  std::array<char, 512> output{};
  auto built = BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    "dGhlIHNhbXBsZSBub25jZQ==", output);
  if (!built.ok) {
    return 1;
  }
  return built.bytes.find("Upgrade: websocket") != std::string_view::npos ? 0
                                                                           : 1;
}
