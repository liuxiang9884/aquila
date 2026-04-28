#ifndef AQUILA_CORE_WEBSOCKET_QUEUED_FRAME_CODEC_H_
#define AQUILA_CORE_WEBSOCKET_QUEUED_FRAME_CODEC_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>

#include "core/websocket/frame_codec_types.h"
#include "core/websocket/frame_parser.h"
#include "core/websocket/mirrored_buffer.h"

namespace aquila::websocket {

class QueuedFrameCodec {
 public:
  explicit QueuedFrameCodec(size_t max_payload_bytes)
      : QueuedFrameCodec(max_payload_bytes,
                         max_payload_bytes >
                                 std::numeric_limits<size_t>::max() -
                                     detail::kMaxFrameHeaderBytes
                             ? max_payload_bytes
                             : max_payload_bytes +
                                   detail::kMaxFrameHeaderBytes,
                         kDefaultReadyFrameSlots) {}

  QueuedFrameCodec(size_t max_payload_bytes, size_t receive_buffer_bytes,
                   size_t ready_frame_slots = kDefaultReadyFrameSlots)
      : max_payload_bytes_(max_payload_bytes),
        ready_frames_(ready_frame_slots == 0
                          ? nullptr
                          : std::make_unique<ReadyFrame[]>(ready_frame_slots)),
        ready_capacity_(ready_frame_slots) {
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
    ready_head_ = 0;
    ready_count_ = 0;
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

 private:
  static constexpr size_t kDefaultReadyFrameSlots = 1024;

  struct ReadyFrame {
    PayloadKind kind{PayloadKind::kText};
    std::uint64_t sequence{0};
    std::uint64_t frame_abs{0};
    std::uint64_t frame_size{0};
    std::uint64_t payload_abs{0};
    size_t payload_size{0};
  };

  DecodeResult PollWithoutRelease() noexcept {
    if (!ReadyEmpty()) {
      return DrainReadyFrame();
    }
    if (protocol_error_pending_) {
      protocol_error_pending_ = false;
      return {DecodeStatus::kProtocolError, {}};
    }
    if (capacity_error_pending_) {
      capacity_error_pending_ = false;
      return {DecodeStatus::kCapacityExceeded, {}};
    }

    ParseReadyFrames();
    if (!ReadyEmpty()) {
      return DrainReadyFrame();
    }
    if (protocol_error_pending_) {
      protocol_error_pending_ = false;
      return {DecodeStatus::kProtocolError, {}};
    }
    if (capacity_error_pending_) {
      capacity_error_pending_ = false;
      return {DecodeStatus::kCapacityExceeded, {}};
    }
    return {};
  }

  void ParseReadyFrames() noexcept {
    if (!receive_ring_.valid()) {
      capacity_error_pending_ = true;
      return;
    }

    for (;;) {
      if (ReadyFull()) {
        capacity_error_pending_ = true;
        return;
      }

      const std::uint64_t available = write_abs_ - parse_abs_;
      const auto parsed = detail::ParseServerFrameHeader(Ptr(parse_abs_),
                                                         available);
      if (parsed.status == detail::FrameHeaderStatus::kNeedMore) {
        return;
      }
      if (parsed.status == detail::FrameHeaderStatus::kProtocolError) {
        protocol_error_pending_ = true;
        return;
      }
      if (parsed.header.payload_length > max_payload_bytes_ ||
          parsed.header.payload_length >
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::uint32_t>::max())) {
        protocol_error_pending_ = true;
        return;
      }

      const std::uint64_t total_frame_bytes =
          parsed.header.header_bytes + parsed.header.payload_length;
      if (available < total_frame_bytes) {
        return;
      }
      if (total_frame_bytes > receive_ring_.capacity()) {
        capacity_error_pending_ = true;
        return;
      }

      const std::uint64_t frame_abs = parse_abs_;
      const std::uint64_t payload_abs =
          frame_abs + parsed.header.header_bytes;
      QueueReadyFrame(ReadyFrame{
          .kind = parsed.header.kind,
          .sequence = next_sequence_++,
          .frame_abs = frame_abs,
          .frame_size = total_frame_bytes,
          .payload_abs = payload_abs,
          .payload_size = static_cast<size_t>(parsed.header.payload_length),
      });
      if (capacity_error_pending_) {
        return;
      }
      parse_abs_ = frame_abs + total_frame_bytes;
    }
  }

  void QueueReadyFrame(const ReadyFrame& frame) noexcept {
    if (ReadyFull()) {
      capacity_error_pending_ = true;
      return;
    }
    const size_t slot = (ready_head_ + ready_count_) % ready_capacity_;
    ready_frames_[slot] = frame;
    ++ready_count_;
  }

  DecodeResult DrainReadyFrame() noexcept {
    const ReadyFrame frame = ready_frames_[ready_head_];
    ready_head_ = (ready_head_ + 1) % ready_capacity_;
    --ready_count_;
    delivered_pending_ = true;
    delivered_frame_end_abs_ = frame.frame_abs + frame.frame_size;

    MessageView view{};
    view.kind = frame.kind;
    view.payload =
        std::span<const std::byte>(Ptr(frame.payload_abs), frame.payload_size);
    view.readable_tail_bytes =
        ReadableTailBytesAfterPayload(frame.payload_abs, frame.payload_size);
    view.sequence = frame.sequence;
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

  std::uint32_t ReadableTailBytesAfterPayload(
      std::uint64_t payload_abs, size_t payload_size) const noexcept {
    if (!receive_ring_.valid()) {
      return 0;
    }
    const size_t payload_offset =
        static_cast<size_t>(payload_abs & receive_mask_);
    const size_t payload_end_offset = payload_offset + payload_size;
    const size_t mapped_bytes = receive_ring_.capacity() * 2U;
    const size_t tail_bytes =
        payload_end_offset < mapped_bytes ? mapped_bytes - payload_end_offset
                                          : 0;
    return tail_bytes > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(tail_bytes);
  }

  bool ReadyEmpty() const noexcept { return ready_count_ == 0; }

  bool ReadyFull() const noexcept {
    return ready_capacity_ == 0 || ready_frames_ == nullptr ||
           ready_count_ == ready_capacity_;
  }

  size_t max_payload_bytes_{0};
  MirroredBuffer receive_ring_{};
  size_t receive_mask_{0};
  std::unique_ptr<ReadyFrame[]> ready_frames_{};
  size_t ready_capacity_{0};
  size_t ready_head_{0};
  size_t ready_count_{0};
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

#endif  // AQUILA_CORE_WEBSOCKET_QUEUED_FRAME_CODEC_H_
