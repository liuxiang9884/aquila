#ifndef AQUILA_CORE_WEBSOCKET_FRAME_CODEC_TYPES_H_
#define AQUILA_CORE_WEBSOCKET_FRAME_CODEC_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/websocket/message_view.h"

namespace aquila::websocket {

struct EncodeResult {
  bool ok{false};
  std::span<const std::byte> bytes{};
};

enum class DecodeStatus : std::uint8_t {
  kNeedMore,
  kMessageReady,
  kProtocolError,
  kCapacityExceeded,
};

struct DecodeResult {
  DecodeStatus status{DecodeStatus::kNeedMore};
  MessageView view{};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_FRAME_CODEC_TYPES_H_
