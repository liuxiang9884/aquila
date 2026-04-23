#ifndef AQUILA_CORE_WEBSOCKET_FRAME_CODEC_H_
#define AQUILA_CORE_WEBSOCKET_FRAME_CODEC_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "core/websocket/message_view.h"

namespace aquila::websocket {

struct EncodeResult {
  bool ok{false};
  std::span<const std::byte> bytes{};
};

enum class DecodeStatus : uint8_t { kNeedMore, kMessageReady, kProtocolError };

struct DecodeResult {
  DecodeStatus status{DecodeStatus::kNeedMore};
  MessageView view{};
};

class FrameCodec {
 public:
  explicit FrameCodec(size_t max_payload_bytes)
      : max_payload_bytes_(max_payload_bytes) {
    inbound_bytes_.reserve(max_payload_bytes_ + 14);
    decoded_payload_.reserve(max_payload_bytes_);
  }

  EncodeResult EncodeText(std::span<const std::byte> payload,
                          std::span<std::byte> output) noexcept {
    return EncodeDataFrame(kOpcodeText, payload, output);
  }

  EncodeResult EncodeBinary(std::span<const std::byte> payload,
                            std::span<std::byte> output) noexcept {
    return EncodeDataFrame(kOpcodeBinary, payload, output);
  }

  EncodeResult EncodeControl(PayloadKind kind,
                             std::span<const std::byte> payload,
                             std::span<std::byte> output) noexcept {
    std::uint8_t opcode = 0;
    switch (kind) {
      case PayloadKind::kPing:
        opcode = kOpcodePing;
        break;
      case PayloadKind::kPong:
        opcode = kOpcodePong;
        break;
      case PayloadKind::kClose:
        opcode = kOpcodeClose;
        break;
      default:
        return {};
    }

    if (payload.size() > kControlPayloadLimit) {
      return {};
    }
    return EncodeFrame(opcode, payload, output);
  }

  DecodeResult Feed(std::span<const std::byte> bytes) noexcept {
    if (inbound_bytes_.size() + bytes.size() > max_payload_bytes_ + 14) {
      return {DecodeStatus::kProtocolError, {}};
    }
    if (!bytes.empty()) {
      inbound_bytes_.insert(inbound_bytes_.end(), bytes.begin(), bytes.end());
    }

    const auto parsed = TryParseNextFrame();
    if (parsed.status != DecodeStatus::kMessageReady) {
      return parsed;
    }

    MessageView view = parsed.view;
    view.payload = std::span<const std::byte>(decoded_payload_.data(),
                                              decoded_payload_.size());
    return {DecodeStatus::kMessageReady, view};
  }

 private:
  static constexpr std::uint8_t kOpcodeText = 0x1;
  static constexpr std::uint8_t kOpcodeBinary = 0x2;
  static constexpr std::uint8_t kOpcodeClose = 0x8;
  static constexpr std::uint8_t kOpcodePing = 0x9;
  static constexpr std::uint8_t kOpcodePong = 0xA;
  static constexpr size_t kControlPayloadLimit = 125;
  static constexpr std::array<std::byte, 4> kMaskKey{
      std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};

  EncodeResult EncodeDataFrame(std::uint8_t opcode,
                               std::span<const std::byte> payload,
                               std::span<std::byte> output) noexcept {
    if (payload.size() > max_payload_bytes_) {
      return {};
    }
    return EncodeFrame(opcode, payload, output);
  }

  EncodeResult EncodeFrame(std::uint8_t opcode,
                           std::span<const std::byte> payload,
                           std::span<std::byte> output) noexcept {
    if (payload.size() > max_payload_bytes_) {
      return {};
    }

    const size_t extended_length_bytes =
        payload.size() <= 125 ? 0 : (payload.size() <= 0xFFFF ? 2 : 8);
    const size_t frame_bytes = 2 + extended_length_bytes + kMaskKey.size() +
                               payload.size();
    if (output.size() < frame_bytes) {
      return {};
    }

    size_t cursor = 0;
    output[cursor++] = std::byte{static_cast<unsigned char>(0x80U | opcode)};
    if (payload.size() <= 125) {
      output[cursor++] = std::byte{static_cast<unsigned char>(0x80U |
                                                           payload.size())};
    } else if (payload.size() <= 0xFFFF) {
      output[cursor++] = std::byte{0xFE};
      output[cursor++] = std::byte{
          static_cast<unsigned char>((payload.size() >> 8) & 0xFFU)};
      output[cursor++] = std::byte{
          static_cast<unsigned char>(payload.size() & 0xFFU)};
    } else {
      output[cursor++] = std::byte{0xFF};
      for (int shift = 56; shift >= 0; shift -= 8) {
        output[cursor++] = std::byte{static_cast<unsigned char>(
            (static_cast<std::uint64_t>(payload.size()) >> shift) & 0xFFU)};
      }
    }

    for (const auto mask_byte : kMaskKey) {
      output[cursor++] = mask_byte;
    }
    for (size_t i = 0; i < payload.size(); ++i) {
      output[cursor++] = payload[i] ^ kMaskKey[i & 0x3U];
    }

    return {true, std::span<const std::byte>(output.data(), frame_bytes)};
  }

  DecodeResult TryParseNextFrame() noexcept {
    if (inbound_bytes_.size() < 2) {
      return {};
    }

    const std::uint8_t first =
        std::to_integer<std::uint8_t>(inbound_bytes_[0]);
    const std::uint8_t second =
        std::to_integer<std::uint8_t>(inbound_bytes_[1]);

    const bool fin = (first & 0x80U) != 0;
    if (!fin || (first & 0x70U) != 0) {
      return {DecodeStatus::kProtocolError, {}};
    }

    const std::uint8_t opcode = first & 0x0FU;
    const auto payload_kind = MapOpcode(opcode);
    if (!payload_kind.has_value()) {
      return {DecodeStatus::kProtocolError, {}};
    }

    size_t cursor = 2;
    std::uint64_t payload_length = second & 0x7FU;
    if (payload_length == 126) {
      if (inbound_bytes_.size() < cursor + 2) {
        return {};
      }
      payload_length =
          (static_cast<std::uint64_t>(
               std::to_integer<std::uint8_t>(inbound_bytes_[2]))
           << 8) |
          static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(inbound_bytes_[3]));
      cursor += 2;
    } else if (payload_length == 127) {
      if (inbound_bytes_.size() < cursor + 8) {
        return {};
      }
      payload_length = 0;
      for (size_t i = 0; i < 8; ++i) {
        payload_length =
            (payload_length << 8) |
            static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(inbound_bytes_[cursor + i]));
      }
      cursor += 8;
      if ((payload_length >> 63U) != 0) {
        return {DecodeStatus::kProtocolError, {}};
      }
    }

    if (payload_length > max_payload_bytes_ ||
        payload_length > static_cast<std::uint64_t>(std::numeric_limits<size_t>::max())) {
      return {DecodeStatus::kProtocolError, {}};
    }
    if (IsControl(*payload_kind) && payload_length > kControlPayloadLimit) {
      return {DecodeStatus::kProtocolError, {}};
    }

    const bool masked = (second & 0x80U) != 0;
    const size_t mask_bytes = masked ? 4 : 0;
    const size_t total_frame_bytes =
        cursor + mask_bytes + static_cast<size_t>(payload_length);
    if (inbound_bytes_.size() < total_frame_bytes) {
      return {};
    }

    std::array<std::byte, 4> mask{};
    if (masked) {
      for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = inbound_bytes_[cursor + i];
      }
      cursor += mask.size();
    }

    decoded_payload_.resize(static_cast<size_t>(payload_length));
    for (size_t i = 0; i < decoded_payload_.size(); ++i) {
      const auto wire_byte = inbound_bytes_[cursor + i];
      decoded_payload_[i] =
          masked ? (wire_byte ^ mask[i & 0x3U]) : wire_byte;
    }

    inbound_bytes_.erase(inbound_bytes_.begin(),
                         inbound_bytes_.begin() + total_frame_bytes);

    MessageView view{};
    view.kind = *payload_kind;
    view.payload = std::span<const std::byte>(decoded_payload_.data(),
                                              decoded_payload_.size());
    view.sequence = next_sequence_++;
    view.fin = true;
    return {DecodeStatus::kMessageReady, view};
  }

  static bool IsControl(PayloadKind kind) noexcept {
    return kind == PayloadKind::kPing || kind == PayloadKind::kPong ||
           kind == PayloadKind::kClose;
  }

  static std::optional<PayloadKind> MapOpcode(std::uint8_t opcode) noexcept {
    switch (opcode) {
      case kOpcodeText:
        return PayloadKind::kText;
      case kOpcodeBinary:
        return PayloadKind::kBinary;
      case kOpcodeClose:
        return PayloadKind::kClose;
      case kOpcodePing:
        return PayloadKind::kPing;
      case kOpcodePong:
        return PayloadKind::kPong;
      default:
        return std::nullopt;
    }
  }

  size_t max_payload_bytes_{0};
  std::uint64_t next_sequence_{0};
  std::vector<std::byte> inbound_bytes_{};
  std::vector<std::byte> decoded_payload_{};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_FRAME_CODEC_H_
