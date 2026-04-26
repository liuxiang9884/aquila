#ifndef AQUILA_CORE_WEBSOCKET_FRAME_CODEC_H_
#define AQUILA_CORE_WEBSOCKET_FRAME_CODEC_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include <openssl/rand.h>

#include "core/websocket/frame_codec_types.h"
#include "core/websocket/frame_parser.h"
#include "core/websocket/mirrored_buffer.h"

namespace aquila::websocket {

class FrameCodec {
 public:
  explicit FrameCodec(size_t max_payload_bytes)
      : FrameCodec(max_payload_bytes,
                   max_payload_bytes >
                           std::numeric_limits<size_t>::max() -
                               detail::kMaxFrameHeaderBytes
                       ? max_payload_bytes
                       : max_payload_bytes + detail::kMaxFrameHeaderBytes) {}

  FrameCodec(size_t max_payload_bytes, size_t receive_buffer_bytes)
      : max_payload_bytes_(max_payload_bytes) {
    const size_t required_capacity =
        max_payload_bytes > std::numeric_limits<size_t>::max() -
                                detail::kMaxFrameHeaderBytes
            ? max_payload_bytes
            : max_payload_bytes + detail::kMaxFrameHeaderBytes;
    receive_ring_.Init(std::max(receive_buffer_bytes, required_capacity));
    receive_mask_ =
        receive_ring_.valid() ? receive_ring_.capacity() - 1U : 0U;
  }

  void Reset() noexcept {
    consume_abs_ = 0;
    parse_abs_ = 0;
    write_abs_ = 0;
    next_sequence_ = 0;
    protocol_error_pending_ = false;
    capacity_error_pending_ = false;
    delivered_pending_ = false;
    delivered_frame_end_abs_ = 0;
  }

  size_t ReceiveCapacity() const noexcept { return receive_ring_.capacity(); }

  std::span<std::byte> WritableSpan() noexcept {
    ReleaseDeliveredFrame();
    if (!receive_ring_.valid()) {
      capacity_error_pending_ = true;
      return {};
    }
    const std::uint64_t used = write_abs_ - consume_abs_;
    if (used >= receive_ring_.capacity()) {
      capacity_error_pending_ = true;
      return {};
    }
    const size_t writable =
        receive_ring_.capacity() - static_cast<size_t>(used);
    return std::span<std::byte>(Ptr(write_abs_), writable);
  }

  void CommitWritten(size_t bytes) noexcept {
    if (!receive_ring_.valid()) {
      capacity_error_pending_ = true;
      return;
    }
    const std::uint64_t used = write_abs_ - consume_abs_;
    const size_t writable =
        used >= receive_ring_.capacity()
            ? 0
            : receive_ring_.capacity() - static_cast<size_t>(used);
    if (bytes > writable) {
      capacity_error_pending_ = true;
      return;
    }
    write_abs_ += bytes;
  }

  DecodeResult Feed(std::span<const std::byte> bytes) noexcept {
    ReleaseDeliveredFrame();
    if (!bytes.empty()) {
      auto writable = WritableSpanWithoutRelease();
      if (bytes.size() > writable.size()) {
        capacity_error_pending_ = true;
      } else {
        std::memcpy(writable.data(), bytes.data(), bytes.size());
        write_abs_ += bytes.size();
      }
    }
    return PollWithoutRelease();
  }

  DecodeResult Poll() noexcept {
    ReleaseDeliveredFrame();
    return PollWithoutRelease();
  }

  EncodeResult EncodeText(std::span<const std::byte> payload,
                          std::span<std::byte> output) noexcept {
    return EncodeDataFrame(detail::kOpcodeText, payload, output);
  }

  EncodeResult EncodeBinary(std::span<const std::byte> payload,
                            std::span<std::byte> output) noexcept {
    return EncodeDataFrame(detail::kOpcodeBinary, payload, output);
  }

  EncodeResult EncodeControl(PayloadKind kind,
                             std::span<const std::byte> payload,
                             std::span<std::byte> output) noexcept {
    std::uint8_t opcode = 0;
    switch (kind) {
      case PayloadKind::kPing:
        opcode = detail::kOpcodePing;
        break;
      case PayloadKind::kPong:
        opcode = detail::kOpcodePong;
        break;
      case PayloadKind::kClose:
        opcode = detail::kOpcodeClose;
        break;
      default:
        return {};
    }

    if (payload.size() > detail::kControlPayloadLimit) {
      return {};
    }
    return EncodeFrame(opcode, payload, output);
  }

 private:
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
    std::array<std::byte, 4> mask_key{};
    if (RAND_bytes(reinterpret_cast<unsigned char*>(mask_key.data()),
                   static_cast<int>(mask_key.size())) != 1) {
      return {};
    }

    const size_t frame_bytes = 2 + extended_length_bytes + mask_key.size() +
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

    for (const auto mask_byte : mask_key) {
      output[cursor++] = mask_byte;
    }
    for (size_t i = 0; i < payload.size(); ++i) {
      output[cursor++] = payload[i] ^ mask_key[i & 0x3U];
    }

    return {true, std::span<const std::byte>(output.data(), frame_bytes)};
  }

  DecodeResult PollWithoutRelease() noexcept {
    if (protocol_error_pending_) {
      protocol_error_pending_ = false;
      return {DecodeStatus::kProtocolError, {}};
    }
    if (capacity_error_pending_) {
      capacity_error_pending_ = false;
      return {DecodeStatus::kCapacityExceeded, {}};
    }

    return ParseOneFrameDirect();
  }

  DecodeResult ParseOneFrameDirect() noexcept {
    if (!receive_ring_.valid()) {
      return {DecodeStatus::kCapacityExceeded, {}};
    }

    const std::uint64_t available = write_abs_ - parse_abs_;
    if (available < 2) {
      return {};
    }

    const auto* data = Ptr(parse_abs_);
    const std::uint8_t first = std::to_integer<std::uint8_t>(data[0]);
    const std::uint8_t second = std::to_integer<std::uint8_t>(data[1]);
    if (first == (0x80U | detail::kOpcodeText) ||
        first == (0x80U | detail::kOpcodeBinary)) {
      const PayloadKind kind = first == (0x80U | detail::kOpcodeText)
                                   ? PayloadKind::kText
                                   : PayloadKind::kBinary;
      if (second < 126U) {
        return BuildDirectMessage(kind, 2, second, available);
      }
      if (second == 126U) {
        if (available < 4U) {
          return {};
        }
        const std::uint64_t payload_length = detail::ReadU16(data + 2);
        if (payload_length < 126U) {
          return {DecodeStatus::kProtocolError, {}};
        }
        return BuildDirectMessage(kind, 4, payload_length, available);
      }
    }

    const auto parsed = detail::ParseServerFrameHeader(data, available);
    if (parsed.status == detail::FrameHeaderStatus::kNeedMore) {
      return {};
    }
    if (parsed.status == detail::FrameHeaderStatus::kProtocolError) {
      return {DecodeStatus::kProtocolError, {}};
    }

    return BuildDirectMessage(parsed.header.kind, parsed.header.header_bytes,
                              parsed.header.payload_length, available);
  }

  DecodeResult BuildDirectMessage(PayloadKind payload_kind, size_t cursor,
                                  std::uint64_t payload_length,
                                  std::uint64_t available) noexcept {
    if (payload_length > max_payload_bytes_ ||
        payload_length >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max())) {
      return {DecodeStatus::kProtocolError, {}};
    }

    const std::uint64_t total_frame_bytes = cursor + payload_length;
    if (available < total_frame_bytes) {
      return {};
    }
    if (total_frame_bytes > receive_ring_.capacity()) {
      return {DecodeStatus::kCapacityExceeded, {}};
    }

    const std::uint64_t frame_abs = parse_abs_;
    const std::uint64_t payload_abs = frame_abs + cursor;
    const std::uint64_t frame_end_abs = frame_abs + total_frame_bytes;
    parse_abs_ = frame_end_abs;
    delivered_pending_ = true;
    delivered_frame_end_abs_ = frame_end_abs;

    MessageView view{};
    view.kind = payload_kind;
    view.payload = std::span<const std::byte>(
        Ptr(payload_abs), static_cast<size_t>(payload_length));
    view.sequence = next_sequence_++;
    view.fin = true;
    return {DecodeStatus::kMessageReady, view};
  }

  void ReleaseDeliveredFrame() noexcept {
    if (!delivered_pending_) {
      return;
    }
    if (delivered_frame_end_abs_ > consume_abs_) {
      consume_abs_ = delivered_frame_end_abs_;
    }
    delivered_pending_ = false;
  }

  std::span<std::byte> WritableSpanWithoutRelease() noexcept {
    if (!receive_ring_.valid()) {
      return {};
    }
    const std::uint64_t used = write_abs_ - consume_abs_;
    if (used >= receive_ring_.capacity()) {
      return {};
    }
    const size_t writable =
        receive_ring_.capacity() - static_cast<size_t>(used);
    return std::span<std::byte>(Ptr(write_abs_), writable);
  }

  std::byte* Ptr(std::uint64_t absolute) noexcept {
    return receive_ring_.data() + (absolute & receive_mask_);
  }

  const std::byte* Ptr(std::uint64_t absolute) const noexcept {
    return receive_ring_.data() + (absolute & receive_mask_);
  }

  size_t max_payload_bytes_{0};
  MirroredBuffer receive_ring_{};
  size_t receive_mask_{0};
  std::uint64_t consume_abs_{0};
  std::uint64_t parse_abs_{0};
  std::uint64_t write_abs_{0};
  std::uint64_t next_sequence_{0};
  bool protocol_error_pending_{false};
  bool capacity_error_pending_{false};
  bool delivered_pending_{false};
  std::uint64_t delivered_frame_end_abs_{0};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_FRAME_CODEC_H_
