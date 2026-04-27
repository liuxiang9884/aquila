#ifndef AQUILA_CORE_WEBSOCKET_CRITICAL_SESSION_H_
#define AQUILA_CORE_WEBSOCKET_CRITICAL_SESSION_H_

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <sys/types.h>

#include "core/websocket/frame_codec.h"
#include "core/websocket/message_view.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/types.h"

namespace aquila::websocket {

template <typename TlsSocketT>
class CriticalSession {
 public:
  CriticalSession(const ConnectionConfig& config, TlsSocketT& tls_socket,
                  PreparedWriteArena& prepared_write_arena,
                  Metrics& metrics) noexcept
      : config_(config),
        tls_socket_(tls_socket),
        prepared_write_arena_(prepared_write_arena),
        metrics_(metrics),
        codec_(config.max_frame_payload_bytes, config.frame_buffer_bytes),
        pending_writes_(config.prepared_write_slots == 0
                            ? nullptr
                            : std::make_unique<PreparedWrite*[]>(
                                  config.prepared_write_slots)),
        pending_capacity_(config.prepared_write_slots) {
    control_write_.storage = std::span<std::byte>(control_write_storage_);
  }

  void SetConsumer(MessageConsumer consumer) noexcept { consumer_ = consumer; }

  PreparedWrite* TryAcquirePreparedWrite() noexcept {
    return prepared_write_arena_.TryAcquire();
  }

  SendStatus CommitPreparedWrite(PreparedWrite* write) noexcept {
    if (write == nullptr) {
      return SendStatus::kNoPreparedWriteSlot;
    }
    if (write->encoded_size > write->storage.size()) {
      return SendStatus::kPayloadTooLarge;
    }
    if (pending_capacity_ == 0 || pending_count_ == pending_capacity_) {
      return SendStatus::kWriteUnavailable;
    }

    pending_writes_[(pending_head_ + pending_count_) % pending_capacity_] = write;
    ++pending_count_;
    metrics_.prepared_write_high_watermark =
        std::max(metrics_.prepared_write_high_watermark,
                 static_cast<std::uint64_t>(pending_count_));
    return SendStatus::kOk;
  }

  void CancelPreparedWrite(PreparedWrite* write) noexcept {
    if (write == nullptr) {
      return;
    }
    for (size_t i = 0; i < pending_count_; ++i) {
      const size_t slot = (pending_head_ + i) % pending_capacity_;
      if (pending_writes_[slot] != write) {
        continue;
      }
      for (size_t shift = i; shift + 1 < pending_count_; ++shift) {
        const size_t from = (pending_head_ + shift + 1) % pending_capacity_;
        const size_t to = (pending_head_ + shift) % pending_capacity_;
        pending_writes_[to] = pending_writes_[from];
      }
      pending_writes_[(pending_head_ + pending_count_ - 1) % pending_capacity_] =
          nullptr;
      --pending_count_;
      prepared_write_arena_.Release(write);
      return;
    }
    prepared_write_arena_.Release(write);
  }

  void DriveWrite() noexcept {
    if (HasPartialBusinessWrite()) {
      DriveBusinessWrites(1);
      if (should_reconnect_ || HasPartialBusinessWrite()) {
        return;
      }
    }

    if (control_write_pending_) {
      DriveControlWrite();
      if (should_reconnect_ || control_write_pending_) {
        return;
      }
    }

    DriveBusinessWrites(MaxBusinessWritesPerDrive());
  }

  void DriveBusinessWrites(std::uint32_t max_completed_writes) noexcept {
    std::uint32_t completed_writes = 0;
    while (pending_count_ != 0) {
      PreparedWrite* write = pending_writes_[pending_head_];
      if (write == nullptr || write->write_offset > write->encoded_size) {
        TriggerReconnect(ConnectionError::kSocketError);
        return;
      }

      const size_t remaining_bytes =
          static_cast<size_t>(write->encoded_size - write->write_offset);
      if (remaining_bytes == 0) {
        CompleteFrontWrite();
        continue;
      }

      const std::span<const std::byte> payload(write->storage.data() +
                                                   write->write_offset,
                                               remaining_bytes);
      const ssize_t written = tls_socket_.WriteSome(payload);
      if (written > 0) {
        write->write_offset += static_cast<std::uint32_t>(written);
        metrics_.tx_bytes += static_cast<std::uint64_t>(written);
        if (write->write_offset == write->encoded_size) {
          CompleteFrontWrite();
          ++completed_writes;
          if (max_completed_writes != 0 &&
              completed_writes >= max_completed_writes) {
            return;
          }
        }
        continue;
      }
      if (written < 0 && errno == EAGAIN) {
        return;
      }
      TriggerReconnect(written == 0 ? ConnectionError::kPeerClosed
                                   : ConnectionError::kSocketError);
      return;
    }
  }

  void DriveRead() noexcept {
    std::uint32_t reads_done = 0;
    while (reads_done < MaxReadsPerDrive()) {
      auto read_buffer = codec_.WritableSpan();
      if (read_buffer.empty()) {
        ++metrics_.frame_codec_capacity_exhaustions;
        return;
      }

      const ssize_t received = tls_socket_.ReadSome(read_buffer);
      if (received > 0) {
        ++reads_done;
        codec_.CommitWritten(static_cast<size_t>(received));
        metrics_.rx_bytes += static_cast<std::uint64_t>(received);
        DrainDecodedMessages();
        if (should_reconnect_ || !ShouldContinueReadPump(reads_done)) {
          return;
        }
        continue;
      }
      if (received < 0 && errno == EAGAIN) {
        return;
      }
      TriggerReconnect(received == 0 ? ConnectionError::kPeerClosed
                                     : ConnectionError::kSocketError);
      return;
    }
  }

  void FeedReadBytes(std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) {
      return;
    }
    DrainDecodedMessages(codec_.Feed(bytes));
  }

  bool WantsWrite() const noexcept {
    return control_write_pending_ || pending_count_ != 0;
  }

  bool WantsRead() const noexcept { return !should_reconnect_; }

  size_t PendingWriteCount() const noexcept { return pending_count_; }

  size_t PendingWriteCapacity() const noexcept { return pending_capacity_; }

  bool AwaitingPong() const noexcept { return awaiting_pong_; }

  std::uint64_t LastPingNs() const noexcept { return last_ping_ns_; }

  void Reset() noexcept {
    while (pending_count_ != 0) {
      PreparedWrite* write = pending_writes_[pending_head_];
      pending_writes_[pending_head_] = nullptr;
      pending_head_ = (pending_head_ + 1) % pending_capacity_;
      --pending_count_;
      prepared_write_arena_.Release(write);
    }
    pending_head_ = 0;
    control_write_.encoded_size = 0;
    control_write_.write_offset = 0;
    control_write_.kind = PayloadKind::kBinary;
    control_write_pending_ = false;
    control_queued_ns_ = 0;
    codec_.Reset();
    should_reconnect_ = false;
    last_error_ = ConnectionError::kNone;
    awaiting_pong_ = false;
    last_ping_ns_ = 0;
  }

  void AdvanceHeartbeat(std::uint64_t now_ns) noexcept {
    const std::uint64_t interval_ns =
        static_cast<std::uint64_t>(config_.heartbeat_interval_ms) * 1000U * 1000U;
    const std::uint64_t timeout_ns =
        static_cast<std::uint64_t>(config_.heartbeat_timeout_ms) * 1000U * 1000U;
    if (!awaiting_pong_) {
      if (last_ping_ns_ == 0 || now_ns - last_ping_ns_ >= interval_ns) {
        if (!EnqueueControlFrame(PayloadKind::kPing, {}, now_ns)) {
          // Skip this tick; do not mark awaiting_pong_ so the next call
          // retries once a slot frees up.
          ++metrics_.control_frame_enqueue_failures;
          return;
        }
        awaiting_pong_ = true;
        last_ping_ns_ = now_ns;
      }
      return;
    }

    if (now_ns - last_ping_ns_ >= timeout_ns) {
      TriggerReconnect(ConnectionError::kHeartbeatTimeout);
      ++metrics_.heartbeat_timeouts;
    }
  }

  bool ShouldReconnect() const noexcept { return should_reconnect_; }
  ConnectionError LastError() const noexcept { return last_error_; }
  int NativeFd() const noexcept { return tls_socket_.NativeFd(); }

 private:
  void TriggerReconnect(ConnectionError error) noexcept {
    should_reconnect_ = true;
    last_error_ = error;
  }

  void CompleteFrontWrite() noexcept {
    PreparedWrite* write = pending_writes_[pending_head_];
    pending_writes_[pending_head_] = nullptr;
    pending_head_ = pending_capacity_ == 0 ? 0 : (pending_head_ + 1) % pending_capacity_;
    --pending_count_;
    ++metrics_.tx_messages;
    prepared_write_arena_.Release(write);
  }

  bool HasPartialBusinessWrite() const noexcept {
    if (pending_count_ == 0) {
      return false;
    }
    const PreparedWrite* write = pending_writes_[pending_head_];
    return write != nullptr && write->write_offset != 0 &&
           write->write_offset < write->encoded_size;
  }

  void DriveControlWrite() noexcept {
    while (control_write_pending_) {
      if (control_write_.write_offset > control_write_.encoded_size) {
        TriggerReconnect(ConnectionError::kSocketError);
        return;
      }

      const size_t remaining_bytes = static_cast<size_t>(
          control_write_.encoded_size - control_write_.write_offset);
      if (remaining_bytes == 0) {
        CompleteControlWrite();
        return;
      }

      const bool first_write = control_write_.write_offset == 0;
      const std::span<const std::byte> payload(
          control_write_.storage.data() + control_write_.write_offset,
          remaining_bytes);
      const ssize_t written = tls_socket_.WriteSome(payload);
      if (written > 0) {
        if (first_write && control_write_.kind == PayloadKind::kPing &&
            control_queued_ns_ != 0) {
          RecordHeartbeatPingSendDelay();
        }
        control_write_.write_offset += static_cast<std::uint32_t>(written);
        metrics_.tx_bytes += static_cast<std::uint64_t>(written);
        if (control_write_.write_offset == control_write_.encoded_size) {
          CompleteControlWrite();
        }
        continue;
      }
      if (written < 0 && errno == EAGAIN) {
        return;
      }
      TriggerReconnect(written == 0 ? ConnectionError::kPeerClosed
                                    : ConnectionError::kSocketError);
      return;
    }
  }

  void CompleteControlWrite() noexcept {
    control_write_.encoded_size = 0;
    control_write_.write_offset = 0;
    control_write_.kind = PayloadKind::kBinary;
    control_write_pending_ = false;
    control_queued_ns_ = 0;
    ++metrics_.tx_messages;
  }

  void RecordHeartbeatPingSendDelay() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const std::uint64_t now_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    if (now_ns < control_queued_ns_) {
      return;
    }
    const std::uint64_t delay = now_ns - control_queued_ns_;
    metrics_.heartbeat_ping_send_delay_ns = delay;
    metrics_.heartbeat_ping_send_delay_max_ns =
        std::max(metrics_.heartbeat_ping_send_delay_max_ns, delay);
  }

  void HandleDecodeResult(const DecodeResult& decoded) noexcept {
    if (decoded.status == DecodeStatus::kCapacityExceeded) {
      ++metrics_.frame_codec_capacity_exhaustions;
      return;
    }
    if (decoded.status == DecodeStatus::kProtocolError) {
      TriggerReconnect(ConnectionError::kProtocolError);
      return;
    }
    if (decoded.status != DecodeStatus::kMessageReady) {
      return;
    }

    switch (decoded.view.kind) {
      case PayloadKind::kText:
      case PayloadKind::kBinary: {
        const DeliveryResult result = consumer_.Handle(decoded.view);
        if (result == DeliveryResult::kFatal) {
          TriggerReconnect(ConnectionError::kConsumerFatal);
          return;
        }
        if (result == DeliveryResult::kBackpressured) {
          // Drop the frame but keep the connection alive; the control plane
          // is expected to observe the counter and decide on degradation.
          ++metrics_.consumer_backpressure_drops;
          return;
        }
        ++metrics_.rx_messages;
        return;
      }
      case PayloadKind::kPong:
        awaiting_pong_ = false;
        return;
      case PayloadKind::kClose:
        TriggerReconnect(ConnectionError::kPeerClosed);
        return;
      case PayloadKind::kPing:
        if (!EnqueueControlFrame(PayloadKind::kPong, decoded.view.payload)) {
          // Skip the auto-pong but keep the connection; heartbeat-timeout
          // path still catches truly dead peers.
          ++metrics_.control_frame_enqueue_failures;
        }
        return;
    }
  }

  void DrainDecodedMessages() noexcept {
    DrainDecodedMessages(codec_.Poll());
  }

  void DrainDecodedMessages(DecodeResult decoded) noexcept {
    HandleDecodeResult(decoded);
    while (!should_reconnect_ && decoded.status == DecodeStatus::kMessageReady) {
      decoded = codec_.Poll();
      HandleDecodeResult(decoded);
    }
  }

  std::uint32_t MaxReadsPerDrive() const noexcept {
    return config_.max_reads_per_drive == 0 ? 1 : config_.max_reads_per_drive;
  }

  std::uint32_t MaxBusinessWritesPerDrive() const noexcept {
    return config_.max_business_writes_per_drive;
  }

  bool ShouldContinueReadPump(std::uint32_t reads_done) const noexcept {
    if (reads_done >= MaxReadsPerDrive()) {
      return false;
    }
    if (config_.read_until_would_block) {
      return true;
    }
    return tls_socket_.PendingReadableBytes() != 0;
  }

  bool EnqueueControlFrame(PayloadKind kind,
                           std::span<const std::byte> payload,
                           std::uint64_t queued_ns = 0) noexcept {
    if (control_write_pending_) {
      return false;
    }

    control_write_.storage = std::span<std::byte>(control_write_storage_);
    const auto encoded =
        codec_.EncodeControl(kind, payload, control_write_.storage);
    if (!encoded.ok) {
      return false;
    }

    control_write_.encoded_size = static_cast<std::uint32_t>(encoded.bytes.size());
    control_write_.write_offset = 0;
    control_write_.kind = kind;
    control_write_pending_ = true;
    control_queued_ns_ = queued_ns;
    return true;
  }

  ConnectionConfig config_{};
  TlsSocketT& tls_socket_;
  PreparedWriteArena& prepared_write_arena_;
  Metrics& metrics_;
  MessageConsumer consumer_{};
  FrameCodec codec_;
  static constexpr size_t kControlWriteStorageBytes = 131;
  std::array<std::byte, kControlWriteStorageBytes> control_write_storage_{};
  PreparedWrite control_write_{};
  bool control_write_pending_{false};
  std::uint64_t control_queued_ns_{0};
  std::unique_ptr<PreparedWrite*[]> pending_writes_{};
  size_t pending_capacity_{0};
  size_t pending_head_{0};
  size_t pending_count_{0};
  bool should_reconnect_{false};
  ConnectionError last_error_{ConnectionError::kNone};
  bool awaiting_pong_{false};
  std::uint64_t last_ping_ns_{0};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_CRITICAL_SESSION_H_
