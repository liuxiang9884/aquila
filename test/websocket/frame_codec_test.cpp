#include "core/websocket/frame_codec.h"

#include <array>

using namespace aquila::websocket;

int main() {
  FrameCodec codec(1024);
  std::array<std::byte, 128> frame_storage{};
  auto encoded = codec.EncodeText(std::as_bytes(std::span{"tick", 4}),
                                  frame_storage);
  if (!encoded.ok) {
    return 1;
  }
  auto decoded = codec.Feed(std::span(encoded.bytes));
  return decoded.status == DecodeStatus::kMessageReady ? 0 : 1;
}
