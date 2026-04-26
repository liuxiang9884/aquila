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

struct BaseFrameHeader {
  bool fin{false};
  bool rsv_set{false};
  bool masked{false};
  std::uint8_t opcode{0};
  std::uint8_t length_code{0};
  PayloadKind kind{PayloadKind::kText};
  bool known_opcode{false};
};

struct PayloadLengthParseResult {
  FrameHeaderStatus status{FrameHeaderStatus::kNeedMore};
  size_t header_bytes{0};
  std::uint64_t payload_length{0};
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

inline PayloadLengthParseResult ReadyPayloadLength(
    size_t header_bytes, std::uint64_t payload_length) noexcept {
  return {FrameHeaderStatus::kReady, header_bytes, payload_length};
}

inline PayloadLengthParseResult NeedMorePayloadLength() noexcept { return {}; }

inline PayloadLengthParseResult ProtocolErrorPayloadLength() noexcept {
  return {FrameHeaderStatus::kProtocolError, 0, 0};
}

inline std::uint64_t ReadU16(const std::byte* data) noexcept {
  return (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[0]))
          << 8U) |
         static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[1]));
}

inline std::uint64_t ReadU64(const std::byte* data) noexcept {
  std::uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value =
        (value << 8U) |
        static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[i]));
  }
  return value;
}

inline BaseFrameHeader ParseBaseHeader(std::uint8_t first,
                                       std::uint8_t second) noexcept {
  BaseFrameHeader header{};
  header.fin = (first & 0x80U) != 0;
  header.rsv_set = (first & 0x70U) != 0;
  header.masked = (second & 0x80U) != 0;
  header.opcode = first & 0x0FU;
  header.length_code = second & 0x7FU;
  header.known_opcode = MapOpcode(header.opcode, &header.kind);
  return header;
}

inline bool ValidateServerBaseHeader(const BaseFrameHeader& header) noexcept {
  return header.fin && !header.rsv_set && header.known_opcode &&
         !header.masked;
}

inline PayloadLengthParseResult DecodePayloadLength(
    const std::byte* data, std::uint64_t available,
    std::uint8_t length_code) noexcept {
  size_t header_bytes = 2;
  std::uint64_t payload_length = length_code;
  if (payload_length == 126) {
    if (available < header_bytes + 2U) {
      return NeedMorePayloadLength();
    }
    payload_length = ReadU16(data + header_bytes);
    header_bytes += 2;
    if (payload_length < 126U) {
      return ProtocolErrorPayloadLength();
    }
  } else if (payload_length == 127) {
    if (available < header_bytes + 8U) {
      return NeedMorePayloadLength();
    }
    payload_length = ReadU64(data + header_bytes);
    header_bytes += 8;
    if ((payload_length >> 63U) != 0) {
      return ProtocolErrorPayloadLength();
    }
    if (payload_length <= 0xFFFFU) {
      return ProtocolErrorPayloadLength();
    }
  }

  return ReadyPayloadLength(header_bytes, payload_length);
}

inline bool ValidateProtocolPayloadLength(
    PayloadKind kind, std::uint64_t payload_length) noexcept {
  if (IsControl(kind) && payload_length > kControlPayloadLimit) {
    return false;
  }
  return true;
}

inline FrameHeaderParseResult ParseServerFrameHeader(
    const std::byte* data, std::uint64_t available) noexcept {
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
      const std::uint64_t payload_length = ReadU16(data + 2);
      if (payload_length < 126U) {
        return ProtocolErrorHeader();
      }
      return ReadyHeader(kind, 4, payload_length);
    }
  }

  const BaseFrameHeader base = ParseBaseHeader(first, second);
  if (!ValidateServerBaseHeader(base)) {
    return ProtocolErrorHeader();
  }

  const auto length = DecodePayloadLength(data, available, base.length_code);
  if (length.status == FrameHeaderStatus::kNeedMore) {
    return NeedMoreHeader();
  }
  if (length.status == FrameHeaderStatus::kProtocolError) {
    return ProtocolErrorHeader();
  }

  if (!ValidateProtocolPayloadLength(base.kind, length.payload_length)) {
    return ProtocolErrorHeader();
  }

  return ReadyHeader(base.kind, length.header_bytes, length.payload_length);
}

}  // namespace aquila::websocket::detail

#endif  // AQUILA_CORE_WEBSOCKET_FRAME_PARSER_H_
