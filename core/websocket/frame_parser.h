#ifndef AQUILA_CORE_WEBSOCKET_FRAME_PARSER_H_
#define AQUILA_CORE_WEBSOCKET_FRAME_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/websocket/types.h"

namespace aquila::websocket::detail {

inline constexpr std::uint8_t kOpcodeText = 0x1;
inline constexpr std::uint8_t kOpcodeBinary = 0x2;
inline constexpr std::uint8_t kOpcodeClose = 0x8;
inline constexpr std::uint8_t kOpcodePing = 0x9;
inline constexpr std::uint8_t kOpcodePong = 0xA;
inline constexpr size_t kControlPayloadLimit = 125;
inline constexpr size_t kMaxFrameHeaderBytes = 14;

enum class FrameHeaderStatus : std::uint8_t {
  kNeedMore,
  kReady,
  kProtocolError,
};

struct ParsedFrameHeader {
  PayloadKind kind{PayloadKind::kText};
  size_t header_bytes{0};
  std::uint64_t payload_length{0};
};

struct FrameHeaderParseResult {
  FrameHeaderStatus status{FrameHeaderStatus::kNeedMore};
  ParsedFrameHeader header{};
};

inline bool IsControl(PayloadKind kind) noexcept {
  return kind == PayloadKind::kPing || kind == PayloadKind::kPong ||
         kind == PayloadKind::kClose;
}

inline bool MapOpcode(std::uint8_t opcode, PayloadKind* kind) noexcept {
  switch (opcode) {
    case kOpcodeText:
      *kind = PayloadKind::kText;
      return true;
    case kOpcodeBinary:
      *kind = PayloadKind::kBinary;
      return true;
    case kOpcodeClose:
      *kind = PayloadKind::kClose;
      return true;
    case kOpcodePing:
      *kind = PayloadKind::kPing;
      return true;
    case kOpcodePong:
      *kind = PayloadKind::kPong;
      return true;
    default:
      return false;
  }
}

inline FrameHeaderParseResult ReadyHeader(
    PayloadKind kind, size_t header_bytes,
    std::uint64_t payload_length) noexcept {
  return {FrameHeaderStatus::kReady,
          ParsedFrameHeader{.kind = kind,
                            .header_bytes = header_bytes,
                            .payload_length = payload_length}};
}

inline FrameHeaderParseResult NeedMoreHeader() noexcept { return {}; }

inline FrameHeaderParseResult ProtocolErrorHeader() noexcept {
  return {FrameHeaderStatus::kProtocolError, {}};
}

inline FrameHeaderParseResult ParseServerFrameHeader(
    const std::byte* data, std::uint64_t available,
    size_t max_payload_bytes) noexcept {
  if (available < 2) {
    return NeedMoreHeader();
  }

  const std::uint8_t first = std::to_integer<std::uint8_t>(data[0]);
  const std::uint8_t second = std::to_integer<std::uint8_t>(data[1]);

  if (first == (0x80U | kOpcodeText) ||
      first == (0x80U | kOpcodeBinary)) {
    const PayloadKind kind = first == (0x80U | kOpcodeText)
                                 ? PayloadKind::kText
                                 : PayloadKind::kBinary;
    if (second < 126U) {
      return ReadyHeader(kind, 2, second);
    }
    if (second == 126U) {
      if (available < 4U) {
        return NeedMoreHeader();
      }
      const std::uint64_t payload_length =
          (static_cast<std::uint64_t>(
               std::to_integer<std::uint8_t>(data[2]))
           << 8U) |
          static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(data[3]));
      if (payload_length < 126U) {
        return ProtocolErrorHeader();
      }
      return ReadyHeader(kind, 4, payload_length);
    }
  }

  const bool fin = (first & 0x80U) != 0;
  if (!fin || (first & 0x70U) != 0) {
    return ProtocolErrorHeader();
  }

  PayloadKind payload_kind{};
  if (!MapOpcode(first & 0x0FU, &payload_kind)) {
    return ProtocolErrorHeader();
  }
  if ((second & 0x80U) != 0) {
    return ProtocolErrorHeader();
  }

  size_t cursor = 2;
  std::uint64_t payload_length = second & 0x7FU;
  if (payload_length == 126) {
    if (available < cursor + 2) {
      return NeedMoreHeader();
    }
    payload_length =
        (static_cast<std::uint64_t>(
             std::to_integer<std::uint8_t>(data[cursor]))
         << 8U) |
        static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(data[cursor + 1]));
    cursor += 2;
    if (payload_length < 126U) {
      return ProtocolErrorHeader();
    }
  } else if (payload_length == 127) {
    if (available < cursor + 8) {
      return NeedMoreHeader();
    }
    payload_length = 0;
    for (size_t i = 0; i < 8; ++i) {
      payload_length =
          (payload_length << 8U) |
          static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(data[cursor + i]));
    }
    cursor += 8;
    if ((payload_length >> 63U) != 0) {
      return ProtocolErrorHeader();
    }
    if (payload_length <= 0xFFFFU) {
      return ProtocolErrorHeader();
    }
  }

  if (payload_length > max_payload_bytes ||
      payload_length >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::uint32_t>::max())) {
    return ProtocolErrorHeader();
  }
  if (IsControl(payload_kind) && payload_length > kControlPayloadLimit) {
    return ProtocolErrorHeader();
  }

  return ReadyHeader(payload_kind, cursor, payload_length);
}

}  // namespace aquila::websocket::detail

#endif  // AQUILA_CORE_WEBSOCKET_FRAME_PARSER_H_
