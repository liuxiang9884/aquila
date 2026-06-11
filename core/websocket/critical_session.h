#ifndef AQUILA_CORE_WEBSOCKET_CRITICAL_SESSION_H_
#define AQUILA_CORE_WEBSOCKET_CRITICAL_SESSION_H_

#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

#include "absl/container/flat_hash_map.h"
#include "core/websocket/frame_codec.h"
#include "core/websocket/message_view.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/types.h"

namespace aquila::websocket {

template <typename TlsSocketT, typename MessageHandlerT = MessageCallback>
class CriticalSession {
#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
  struct SocketTimestampingProbeState {
    bool active{false};
    std::uint32_t slot_index{kInvalidSocketTimestampingProbeSlot};
    std::uint32_t active_index{kInvalidSocketTimestampingProbeSlot};
    std::uint64_t sequence{0};
    SocketTimestampingSnapshot snapshot{};
    bool has_tx_id_range{false};
    std::uint32_t first_tx_id{0};
    std::uint32_t last_tx_id{0};
    bool has_tx_sched_id{false};
    std::uint32_t tx_sched_id{0};
    bool has_tx_software_id{false};
    std::uint32_t tx_software_id{0};
    bool has_tx_ack_id{false};
    std::uint32_t tx_ack_id{0};
  };
#endif

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
#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
    InitializeSocketTimestampingProbes();
#endif
  }

  void SetMessageHandler(MessageHandlerT message_handler) noexcept {
    message_handler_ = message_handler;
  }

  void SetMessageCallback(MessageCallback message_callback) noexcept
    requires std::is_same_v<MessageHandlerT, MessageCallback>
  {
    SetMessageHandler(message_callback);
  }

  PreparedWrite* TryAcquirePreparedWrite() noexcept {
    return prepared_write_arena_.TryAcquire();
  }

  // Text business writes are cold compared with the read path; keep this
  // helper out-of-line so exchange handlers keep their inlining budget.
  [[gnu::noinline]] SendStatus SendText(
      std::span<const std::byte> payload,
      WriteFlushMode flush_mode = WriteFlushMode::kQueued,
      WritePathDiagnostics* diagnostics = nullptr) noexcept {
    PreparedWrite* write = TryAcquirePreparedWrite();
    if (write == nullptr) {
      ReleasePendingSocketTimestampingProbe();
      return SendStatus::kNoPreparedWriteSlot;
    }

    const EncodeResult encoded = codec_.EncodeText(payload, write->storage);
    if (diagnostics != nullptr) {
      diagnostics->ws_frame_encode_done_ns = RealtimeNowNsInt64();
    }
    if (!encoded.ok) {
      ReleasePendingSocketTimestampingProbe();
      prepared_write_arena_.Release(write);
      return SendStatus::kEncodeFailed;
    }

    write->encoded_size = static_cast<std::uint32_t>(encoded.bytes.size());
    write->write_offset = 0;
    write->kind = PayloadKind::kText;
    write->diagnostics = diagnostics;
    const SendStatus status = CommitPreparedWrite(write, flush_mode);
    if (diagnostics != nullptr) {
      diagnostics->pending_write_count_after = pending_count_;
    }
    if (status != SendStatus::kOk) {
      ReleasePendingSocketTimestampingProbe();
      prepared_write_arena_.Release(write);
    } else if (write->encoded_size != 0 &&
               write->write_offset < write->encoded_size) {
      write->diagnostics = nullptr;
    }
    return status;
  }

  SendStatus CommitPreparedWrite(
      PreparedWrite* write,
      WriteFlushMode flush_mode = WriteFlushMode::kQueued) noexcept {
    if (write == nullptr) {
      ReleasePendingSocketTimestampingProbe();
      return SendStatus::kNoPreparedWriteSlot;
    }
    if (write->encoded_size > write->storage.size()) {
      ReleasePendingSocketTimestampingProbe();
      return SendStatus::kPayloadTooLarge;
    }
    if (pending_capacity_ == 0 || pending_count_ == pending_capacity_) {
      ReleasePendingSocketTimestampingProbe();
      return SendStatus::kWriteUnavailable;
    }

    AttachPendingSocketTimestampingProbe(write);
    pending_writes_[(pending_head_ + pending_count_) % pending_capacity_] =
        write;
    ++pending_count_;
    if (write->diagnostics != nullptr) {
      write->diagnostics->write_enqueue_ns = RealtimeNowNsInt64();
      write->diagnostics->pending_write_count_after = pending_count_;
    }
    metrics_.prepared_write_high_watermark =
        std::max(metrics_.prepared_write_high_watermark,
                 static_cast<std::uint64_t>(pending_count_));
    if (flush_mode == WriteFlushMode::kTryFlushOne && !should_reconnect_ &&
        (!control_write_pending_ || HasPartialBusinessWrite())) {
      TryFlushOneBusinessWrite();
    }
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
      pending_writes_[(pending_head_ + pending_count_ - 1) %
                      pending_capacity_] = nullptr;
      --pending_count_;
      ReleaseSocketTimestampingProbeForWrite(write);
      prepared_write_arena_.Release(write);
      return;
    }
    ReleaseSocketTimestampingProbeForWrite(write);
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
        TriggerReconnect(ConnectionError::kSocketError,
                         ReconnectTrigger::kBusinessWriteInvalidState);
        return;
      }

      const size_t remaining_bytes =
          static_cast<size_t>(write->encoded_size - write->write_offset);
      if (remaining_bytes == 0) {
        RecordWriteComplete(write,
                            pending_count_ == 0 ? 0 : pending_count_ - 1);
        CompleteFrontWrite();
        continue;
      }

      RecordDriveWriteEnter(write);
      const std::span<const std::byte> payload(
          write->storage.data() + write->write_offset, remaining_bytes);
      RecordWriteSomeEnter(write);
      const ssize_t written = tls_socket_.WriteSome(payload);
      RecordWriteSomeReturn(write, written, pending_count_);
      if (written > 0) {
        RecordSocketTimestampingWrite(written, write);
        DrainSocketTimestampingEvents();
        write->write_offset += static_cast<std::uint32_t>(written);
        metrics_.tx_bytes += static_cast<std::uint64_t>(written);
        if (write->write_offset == write->encoded_size) {
          RecordWriteComplete(write,
                              pending_count_ == 0 ? 0 : pending_count_ - 1);
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
      if (written < 0) {
        TriggerReconnect(ConnectionError::kSocketError,
                         ReconnectTrigger::kBusinessWriteError, errno);
        return;
      }
      TriggerReconnect(ConnectionError::kPeerClosed,
                       ReconnectTrigger::kBusinessWriteEof);
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
        RecordSocketRxTimestampingEvent();
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
      if (received < 0) {
        TriggerReconnect(ConnectionError::kSocketError,
                         ReconnectTrigger::kReadError, errno);
        return;
      }
      TriggerReconnect(ConnectionError::kPeerClosed,
                       ReconnectTrigger::kReadEof);
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

  bool WantsRead() const noexcept {
    return !should_reconnect_;
  }

  size_t PendingWriteCount() const noexcept {
    return pending_count_;
  }

  size_t PendingWriteCapacity() const noexcept {
    return pending_capacity_;
  }

  bool AwaitingPong() const noexcept {
    return awaiting_pong_;
  }

  std::uint64_t LastPingNs() const noexcept {
    return last_ping_ns_;
  }

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
#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
    control_write_.socket_timestamping_sequence = 0;
    control_write_.socket_timestamping_probe_slot =
        kInvalidSocketTimestampingProbeSlot;
#endif
    control_write_.kind = PayloadKind::kBinary;
    control_write_pending_ = false;
    control_queued_ns_ = 0;
    codec_.Reset();
    should_reconnect_ = false;
    last_error_ = ConnectionError::kNone;
    reconnect_trigger_ = ReconnectTrigger::kNone;
    reconnect_errno_ = 0;
    awaiting_pong_ = false;
    last_ping_ns_ = 0;
#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
    ResetSocketTimestampingProbes();
#endif
  }

  void AdvanceHeartbeat(std::uint64_t now_ns) noexcept {
    const std::uint64_t interval_ns =
        static_cast<std::uint64_t>(config_.heartbeat_interval_ms) * 1000U *
        1000U;
    const std::uint64_t timeout_ns =
        static_cast<std::uint64_t>(config_.heartbeat_timeout_ms) * 1000U *
        1000U;
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
      TriggerReconnect(ConnectionError::kHeartbeatTimeout,
                       ReconnectTrigger::kHeartbeatTimeout);
      ++metrics_.heartbeat_timeouts;
    }
  }

  bool ShouldReconnect() const noexcept {
    return should_reconnect_;
  }
  ConnectionError LastError() const noexcept {
    return last_error_;
  }
  ReconnectTrigger LastReconnectTrigger() const noexcept {
    return reconnect_trigger_;
  }
  int LastReconnectErrno() const noexcept {
    return reconnect_errno_;
  }
  int NativeFd() const noexcept {
    return tls_socket_.NativeFd();
  }

  bool StartSocketTimestampingProbe(std::uint64_t sequence) noexcept {
#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
    if (!SocketTimestampingAttributionEnabled() || sequence == 0) {
      return false;
    }
    return AllocateSocketTimestampingProbe(sequence);
#else
    (void)sequence;
    return false;
#endif
  }

  void SetSocketTimestampingProbeWriteComplete(
      std::uint64_t sequence, std::int64_t write_complete_ns) noexcept {
#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
    SocketTimestampingProbeState* probe = FindSocketTimestampingProbe(sequence);
    if (probe == nullptr) {
      return;
    }
    probe->snapshot.write_complete_ns = write_complete_ns;
#else
    (void)sequence;
    (void)write_complete_ns;
#endif
  }

  [[nodiscard]] SocketTimestampingSnapshot FinishSocketTimestampingProbe(
      std::uint64_t sequence, std::int64_t ack_receive_local_ns) noexcept {
#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
    DrainSocketTimestampingEvents();
    SocketTimestampingProbeState* probe = FindSocketTimestampingProbe(sequence);
    if (probe == nullptr) {
      return {};
    }
    if (last_socket_rx_software_timestamp_ns_ > 0 &&
        probe->snapshot.rx_software_ns == 0) {
      probe->snapshot.available = true;
      probe->snapshot.rx_software_ns = last_socket_rx_software_timestamp_ns_;
    }
    probe->snapshot.ack_receive_local_ns = ack_receive_local_ns;
    SocketTimestampingSnapshot snapshot = probe->snapshot;
    ReleaseSocketTimestampingProbe(probe->slot_index);
    return snapshot;
#else
    (void)sequence;
    (void)ack_receive_local_ns;
    return {};
#endif
  }

 private:
  [[nodiscard]] static std::int64_t RealtimeNowNsInt64() noexcept {
    return static_cast<std::int64_t>(RealtimeClockNowNs());
  }

  static void RecordDriveWriteEnter(PreparedWrite* write) noexcept {
    if (write == nullptr || write->diagnostics == nullptr ||
        write->diagnostics->drive_write_enter_ns != 0) {
      return;
    }
    write->diagnostics->drive_write_enter_ns = RealtimeNowNsInt64();
  }

  static void RecordWriteSomeEnter(PreparedWrite* write) noexcept {
    if (write == nullptr || write->diagnostics == nullptr ||
        write->diagnostics->write_some_enter_ns != 0) {
      return;
    }
    write->diagnostics->write_some_enter_ns = RealtimeNowNsInt64();
  }

  static void RecordWriteSomeReturn(PreparedWrite* write, ssize_t written,
                                    std::size_t pending_count) noexcept {
    if (write == nullptr || write->diagnostics == nullptr) {
      return;
    }
    WritePathDiagnostics& diagnostics = *write->diagnostics;
    diagnostics.write_some_return_ns = RealtimeNowNsInt64();
    diagnostics.pending_write_count_after = pending_count;
    if (written > 0) {
      diagnostics.write_some_bytes = written;
      diagnostics.write_errno = 0;
      return;
    }
    diagnostics.write_some_bytes = 0;
    diagnostics.write_errno = errno;
    diagnostics.write_eagain = errno == EAGAIN || errno == EWOULDBLOCK;
  }

  static void RecordWriteComplete(PreparedWrite* write,
                                  std::size_t pending_count_after) noexcept {
    if (write == nullptr || write->diagnostics == nullptr) {
      return;
    }
    WritePathDiagnostics& diagnostics = *write->diagnostics;
    diagnostics.write_complete_ns = RealtimeNowNsInt64();
    diagnostics.write_complete_bytes = write->encoded_size;
    diagnostics.pending_write_count_after = pending_count_after;
  }

#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
  [[nodiscard]] bool SocketTimestampingAttributionEnabled() const noexcept {
    if (!config_.socket_timestamping.enabled) {
      return false;
    }
    if constexpr (requires(const TlsSocketT& socket) {
                    socket.timestamping_apply_result();
                  }) {
      const SocketTimestampingApplyResult result =
          tls_socket_.timestamping_apply_result();
      return result.ok && result.enabled;
    }
    return false;
  }

  void InitializeSocketTimestampingProbes() noexcept {
    if (!config_.socket_timestamping.enabled ||
        config_.socket_timestamping.max_active_probes == 0) {
      return;
    }
    socket_timestamping_probe_capacity_ =
        config_.socket_timestamping.max_active_probes;
    socket_timestamping_probes_ =
        std::make_unique<SocketTimestampingProbeState[]>(
            socket_timestamping_probe_capacity_);
    socket_timestamping_free_slots_ =
        std::make_unique<std::uint32_t[]>(socket_timestamping_probe_capacity_);
    socket_timestamping_active_slots_ =
        std::make_unique<std::uint32_t[]>(socket_timestamping_probe_capacity_);
    socket_timestamping_sequence_to_slot_.reserve(
        socket_timestamping_probe_capacity_);
    ResetSocketTimestampingProbes();
  }

  void ResetSocketTimestampingProbeState(std::uint32_t slot_index) noexcept {
    SocketTimestampingProbeState& probe =
        socket_timestamping_probes_[slot_index];
    probe = SocketTimestampingProbeState{};
    probe.slot_index = slot_index;
  }

  void ResetSocketTimestampingProbes() noexcept {
    socket_timestamping_sequence_to_slot_.clear();
    pending_socket_timestamping_sequence_ = 0;
    pending_socket_timestamping_probe_slot_ =
        kInvalidSocketTimestampingProbeSlot;
    socket_timestamping_active_count_ = 0;
    socket_timestamping_free_count_ = socket_timestamping_probe_capacity_;
    socket_timestamping_next_tx_id_ = 0;
    last_socket_rx_software_timestamp_ns_ = 0;
    for (std::uint32_t i = 0; i < socket_timestamping_probe_capacity_; ++i) {
      ResetSocketTimestampingProbeState(i);
      socket_timestamping_free_slots_[i] =
          socket_timestamping_probe_capacity_ - 1U - i;
      socket_timestamping_active_slots_[i] =
          kInvalidSocketTimestampingProbeSlot;
    }
  }

  [[nodiscard]] SocketTimestampingProbeState* FindSocketTimestampingProbe(
      std::uint64_t sequence) noexcept {
    if (sequence == 0) {
      return nullptr;
    }
    const auto it = socket_timestamping_sequence_to_slot_.find(sequence);
    if (it == socket_timestamping_sequence_to_slot_.end()) {
      return nullptr;
    }
    return FindSocketTimestampingProbeBySlot(it->second, sequence);
  }

  [[nodiscard]] const SocketTimestampingProbeState* FindSocketTimestampingProbe(
      std::uint64_t sequence) const noexcept {
    if (sequence == 0) {
      return nullptr;
    }
    const auto it = socket_timestamping_sequence_to_slot_.find(sequence);
    if (it == socket_timestamping_sequence_to_slot_.end()) {
      return nullptr;
    }
    return FindSocketTimestampingProbeBySlot(it->second, sequence);
  }

  [[nodiscard]] SocketTimestampingProbeState* FindSocketTimestampingProbeBySlot(
      std::uint32_t slot_index, std::uint64_t sequence) noexcept {
    if (slot_index == kInvalidSocketTimestampingProbeSlot ||
        slot_index >= socket_timestamping_probe_capacity_) {
      return nullptr;
    }
    SocketTimestampingProbeState& probe =
        socket_timestamping_probes_[slot_index];
    if (!probe.active || probe.sequence != sequence) {
      return nullptr;
    }
    return &probe;
  }

  [[nodiscard]] const SocketTimestampingProbeState*
  FindSocketTimestampingProbeBySlot(std::uint32_t slot_index,
                                    std::uint64_t sequence) const noexcept {
    if (slot_index == kInvalidSocketTimestampingProbeSlot ||
        slot_index >= socket_timestamping_probe_capacity_) {
      return nullptr;
    }
    const SocketTimestampingProbeState& probe =
        socket_timestamping_probes_[slot_index];
    if (!probe.active || probe.sequence != sequence) {
      return nullptr;
    }
    return &probe;
  }

  void ReleaseSocketTimestampingProbe(std::uint32_t slot_index) noexcept {
    if (slot_index == kInvalidSocketTimestampingProbeSlot ||
        slot_index >= socket_timestamping_probe_capacity_) {
      return;
    }
    SocketTimestampingProbeState& probe =
        socket_timestamping_probes_[slot_index];
    if (!probe.active) {
      return;
    }
    if (pending_socket_timestamping_probe_slot_ == slot_index) {
      pending_socket_timestamping_sequence_ = 0;
      pending_socket_timestamping_probe_slot_ =
          kInvalidSocketTimestampingProbeSlot;
    }
    socket_timestamping_sequence_to_slot_.erase(probe.sequence);
    const std::uint32_t active_index = probe.active_index;
    if (active_index < socket_timestamping_active_count_) {
      const std::uint32_t last_active_index =
          socket_timestamping_active_count_ - 1U;
      const std::uint32_t last_slot =
          socket_timestamping_active_slots_[last_active_index];
      socket_timestamping_active_slots_[active_index] = last_slot;
      if (last_slot != kInvalidSocketTimestampingProbeSlot &&
          last_slot < socket_timestamping_probe_capacity_) {
        socket_timestamping_probes_[last_slot].active_index = active_index;
      }
      socket_timestamping_active_slots_[last_active_index] =
          kInvalidSocketTimestampingProbeSlot;
      --socket_timestamping_active_count_;
    }
    ResetSocketTimestampingProbeState(slot_index);
    if (socket_timestamping_free_count_ < socket_timestamping_probe_capacity_) {
      socket_timestamping_free_slots_[socket_timestamping_free_count_++] =
          slot_index;
    }
  }

  void ReleaseSocketTimestampingProbeForWrite(PreparedWrite* write) noexcept {
    if (write == nullptr) {
      return;
    }
    SocketTimestampingProbeState* probe =
        FindSocketTimestampingProbeBySlot(write->socket_timestamping_probe_slot,
                                          write->socket_timestamping_sequence);
    if (probe == nullptr) {
      return;
    }
    ReleaseSocketTimestampingProbe(probe->slot_index);
  }

  void ReleasePendingSocketTimestampingProbe() noexcept {
    if (pending_socket_timestamping_probe_slot_ ==
        kInvalidSocketTimestampingProbeSlot) {
      return;
    }
    ReleaseSocketTimestampingProbe(pending_socket_timestamping_probe_slot_);
  }

  [[nodiscard]] bool AllocateSocketTimestampingProbe(
      std::uint64_t sequence) noexcept {
    if (socket_timestamping_probe_capacity_ == 0 || sequence == 0) {
      return false;
    }
    if (pending_socket_timestamping_probe_slot_ !=
        kInvalidSocketTimestampingProbeSlot) {
      ReleaseSocketTimestampingProbe(pending_socket_timestamping_probe_slot_);
    }
    if (const SocketTimestampingProbeState* existing =
            FindSocketTimestampingProbe(sequence);
        existing != nullptr) {
      ReleaseSocketTimestampingProbe(existing->slot_index);
    }
    if (socket_timestamping_free_count_ == 0 ||
        socket_timestamping_active_count_ >=
            socket_timestamping_probe_capacity_) {
      return false;
    }

    const std::uint32_t slot_index =
        socket_timestamping_free_slots_[--socket_timestamping_free_count_];
    SocketTimestampingProbeState& probe =
        socket_timestamping_probes_[slot_index];
    ResetSocketTimestampingProbeState(slot_index);
    probe.active = true;
    probe.active_index = socket_timestamping_active_count_;
    probe.sequence = sequence;
    probe.snapshot.available = true;
    socket_timestamping_active_slots_[socket_timestamping_active_count_++] =
        slot_index;
    socket_timestamping_sequence_to_slot_[sequence] = slot_index;
    pending_socket_timestamping_sequence_ = sequence;
    pending_socket_timestamping_probe_slot_ = slot_index;
    return true;
  }

  void AttachPendingSocketTimestampingProbe(PreparedWrite* write) noexcept {
    if (write == nullptr || pending_socket_timestamping_probe_slot_ ==
                                kInvalidSocketTimestampingProbeSlot) {
      return;
    }
    SocketTimestampingProbeState* probe = FindSocketTimestampingProbeBySlot(
        pending_socket_timestamping_probe_slot_,
        pending_socket_timestamping_sequence_);
    if (probe == nullptr) {
      pending_socket_timestamping_sequence_ = 0;
      pending_socket_timestamping_probe_slot_ =
          kInvalidSocketTimestampingProbeSlot;
      return;
    }
    write->socket_timestamping_sequence = probe->sequence;
    write->socket_timestamping_probe_slot = probe->slot_index;
    pending_socket_timestamping_sequence_ = 0;
    pending_socket_timestamping_probe_slot_ =
        kInvalidSocketTimestampingProbeSlot;
  }

  void RecordSocketTimestampingWrite(
      ssize_t written, const PreparedWrite* write = nullptr) noexcept {
    if (!config_.socket_timestamping.enabled || written <= 0) {
      return;
    }
    const std::uint32_t bytes = static_cast<std::uint32_t>(written);
    if (bytes == 0) {
      return;
    }
    const std::uint32_t first_tx_id = socket_timestamping_next_tx_id_;
    const std::uint32_t last_tx_id = first_tx_id + bytes - 1U;
    socket_timestamping_next_tx_id_ = last_tx_id + 1U;
    if (write == nullptr) {
      return;
    }
    SocketTimestampingProbeState* probe =
        FindSocketTimestampingProbeBySlot(write->socket_timestamping_probe_slot,
                                          write->socket_timestamping_sequence);
    if (probe == nullptr) {
      return;
    }
    if (!probe->has_tx_id_range) {
      probe->first_tx_id = first_tx_id;
      probe->has_tx_id_range = true;
    }
    probe->last_tx_id = last_tx_id;
  }

  [[nodiscard]] static bool IsSocketTimestampingTxEvent(
      SocketTimestampingEventKind kind) noexcept {
    return kind == SocketTimestampingEventKind::kTxSched ||
           kind == SocketTimestampingEventKind::kTxSoftware ||
           kind == SocketTimestampingEventKind::kTxAck;
  }

  [[nodiscard]] static bool SocketTimestampingIdInRange(
      std::uint32_t id, std::uint32_t first, std::uint32_t last) noexcept {
    if (first <= last) {
      return id >= first && id <= last;
    }
    return id >= first || id <= last;
  }

  [[nodiscard]] static std::uint32_t SocketTimestampingIdDistance(
      std::uint32_t first, std::uint32_t id) noexcept {
    return id - first;
  }

  [[nodiscard]] SocketTimestampingProbeState*
  FindSocketTimestampingProbeForTxEvent(
      const SocketTimestampingEvent& event) noexcept {
    if (!IsSocketTimestampingTxEvent(event.kind)) {
      return nullptr;
    }
    for (std::uint32_t i = 0; i < socket_timestamping_active_count_; ++i) {
      const std::uint32_t slot_index = socket_timestamping_active_slots_[i];
      if (slot_index == kInvalidSocketTimestampingProbeSlot ||
          slot_index >= socket_timestamping_probe_capacity_) {
        continue;
      }
      SocketTimestampingProbeState& probe =
          socket_timestamping_probes_[slot_index];
      if (probe.active && probe.has_tx_id_range &&
          SocketTimestampingIdInRange(event.id, probe.first_tx_id,
                                      probe.last_tx_id)) {
        return &probe;
      }
    }
    return nullptr;
  }

  [[nodiscard]] static bool ShouldRecordSocketTimestampingTxEvent(
      const SocketTimestampingProbeState& probe, std::uint32_t id,
      bool has_recorded_id, std::uint32_t recorded_id) noexcept {
    if (!has_recorded_id) {
      return true;
    }
    return SocketTimestampingIdDistance(probe.first_tx_id, id) >
           SocketTimestampingIdDistance(probe.first_tx_id, recorded_id);
  }
#else
  void ReleaseSocketTimestampingProbeForWrite(PreparedWrite* write) noexcept {
    (void)write;
  }

  void ReleasePendingSocketTimestampingProbe() noexcept {}

  void AttachPendingSocketTimestampingProbe(PreparedWrite* write) noexcept {
    (void)write;
  }

  void RecordSocketTimestampingWrite(
      ssize_t written, const PreparedWrite* write = nullptr) noexcept {
    (void)written;
    (void)write;
  }
#endif

  void TriggerReconnect(ConnectionError error, ReconnectTrigger trigger,
                        int error_number = 0) noexcept {
    should_reconnect_ = true;
    last_error_ = error;
    reconnect_trigger_ = trigger;
    reconnect_errno_ = error_number;
  }

#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
  void DrainSocketTimestampingEvents() noexcept {
    if (!config_.socket_timestamping.enabled) {
      return;
    }
    if constexpr (requires(TlsSocketT& socket) {
                    socket.DrainTimestampingErrorQueue(std::uint32_t{0});
                  }) {
      const SocketTimestampingEventDrain drain =
          tls_socket_.DrainTimestampingErrorQueue(
              config_.socket_timestamping.max_errqueue_events_per_drain);
      if (!drain.ok) {
        return;
      }
      for (std::uint32_t i = 0; i < drain.events_seen; ++i) {
        RecordSocketTimestampingEvent(drain.events[i]);
      }
    }
  }

  void RecordSocketTimestampingEvent(
      const SocketTimestampingEvent& event) noexcept {
    if (event.timestamp_ns <= 0) {
      return;
    }
    SocketTimestampingProbeState* probe =
        FindSocketTimestampingProbeForTxEvent(event);
    if (probe == nullptr) {
      return;
    }
    probe->snapshot.available = true;
    switch (event.kind) {
      case SocketTimestampingEventKind::kTxSched:
        if (ShouldRecordSocketTimestampingTxEvent(
                *probe, event.id, probe->has_tx_sched_id, probe->tx_sched_id)) {
          probe->snapshot.tx_sched_ns = event.timestamp_ns;
          probe->tx_sched_id = event.id;
          probe->has_tx_sched_id = true;
        }
        return;
      case SocketTimestampingEventKind::kTxSoftware:
        if (ShouldRecordSocketTimestampingTxEvent(*probe, event.id,
                                                  probe->has_tx_software_id,
                                                  probe->tx_software_id)) {
          probe->snapshot.tx_software_ns = event.timestamp_ns;
          probe->tx_software_id = event.id;
          probe->has_tx_software_id = true;
        }
        return;
      case SocketTimestampingEventKind::kTxAck:
        if (ShouldRecordSocketTimestampingTxEvent(
                *probe, event.id, probe->has_tx_ack_id, probe->tx_ack_id)) {
          probe->snapshot.tx_ack_ns = event.timestamp_ns;
          probe->tx_ack_id = event.id;
          probe->has_tx_ack_id = true;
        }
        return;
      case SocketTimestampingEventKind::kRxSoftware:
      case SocketTimestampingEventKind::kUnknown:
        return;
    }
  }

  void RecordSocketRxTimestampingEvent() noexcept {
    if (!config_.socket_timestamping.enabled) {
      return;
    }
    if constexpr (requires(TlsSocketT& socket) {
                    socket.TakeLastRxSoftwareTimestampNs();
                  }) {
      last_socket_rx_software_timestamp_ns_ =
          tls_socket_.TakeLastRxSoftwareTimestampNs();
    }
  }
#else
  void DrainSocketTimestampingEvents() noexcept {}

  void RecordSocketRxTimestampingEvent() noexcept {}
#endif

  void CompleteFrontWrite() noexcept {
    PreparedWrite* write = pending_writes_[pending_head_];
    pending_writes_[pending_head_] = nullptr;
    pending_head_ =
        pending_capacity_ == 0 ? 0 : (pending_head_ + 1) % pending_capacity_;
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

  void TryFlushOneBusinessWrite() noexcept {
    while (pending_count_ != 0) {
      PreparedWrite* write = pending_writes_[pending_head_];
      if (write == nullptr || write->write_offset > write->encoded_size) {
        TriggerReconnect(ConnectionError::kSocketError,
                         ReconnectTrigger::kBusinessWriteInvalidState);
        return;
      }

      const size_t remaining_bytes =
          static_cast<size_t>(write->encoded_size - write->write_offset);
      if (remaining_bytes == 0) {
        RecordWriteComplete(write,
                            pending_count_ == 0 ? 0 : pending_count_ - 1);
        CompleteFrontWrite();
        continue;
      }

      RecordDriveWriteEnter(write);
      const std::span<const std::byte> payload(
          write->storage.data() + write->write_offset, remaining_bytes);
      RecordWriteSomeEnter(write);
      const ssize_t written = tls_socket_.WriteSome(payload);
      RecordWriteSomeReturn(write, written, pending_count_);
      if (written > 0) {
        RecordSocketTimestampingWrite(written, write);
        DrainSocketTimestampingEvents();
        write->write_offset += static_cast<std::uint32_t>(written);
        metrics_.tx_bytes += static_cast<std::uint64_t>(written);
        if (write->write_offset == write->encoded_size) {
          RecordWriteComplete(write,
                              pending_count_ == 0 ? 0 : pending_count_ - 1);
          CompleteFrontWrite();
        }
        return;
      }
      if (written < 0 && errno == EAGAIN) {
        return;
      }
      if (written < 0) {
        TriggerReconnect(ConnectionError::kSocketError,
                         ReconnectTrigger::kBusinessWriteError, errno);
        return;
      }
      TriggerReconnect(ConnectionError::kPeerClosed,
                       ReconnectTrigger::kBusinessWriteEof);
      return;
    }
  }

  void DriveControlWrite() noexcept {
    while (control_write_pending_) {
      if (control_write_.write_offset > control_write_.encoded_size) {
        TriggerReconnect(ConnectionError::kSocketError,
                         ReconnectTrigger::kControlWriteInvalidState);
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
        RecordSocketTimestampingWrite(written, nullptr);
        DrainSocketTimestampingEvents();
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
      if (written < 0) {
        TriggerReconnect(ConnectionError::kSocketError,
                         ReconnectTrigger::kControlWriteError, errno);
        return;
      }
      TriggerReconnect(ConnectionError::kPeerClosed,
                       ReconnectTrigger::kControlWriteEof);
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
      TriggerReconnect(ConnectionError::kProtocolError,
                       ReconnectTrigger::kProtocolError);
      return;
    }
    if (decoded.status != DecodeStatus::kMessageReady) {
      return;
    }

    switch (decoded.view.kind) {
      case PayloadKind::kText:
      case PayloadKind::kBinary: {
        const DeliveryResult result = message_handler_.Handle(decoded.view);
        if (result == DeliveryResult::kFatal) {
          TriggerReconnect(ConnectionError::kConsumerFatal,
                           ReconnectTrigger::kConsumerFatal);
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
        TriggerReconnect(ConnectionError::kPeerClosed,
                         ReconnectTrigger::kWebSocketCloseFrame);
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
    while (!should_reconnect_ &&
           decoded.status == DecodeStatus::kMessageReady) {
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

  bool EnqueueControlFrame(PayloadKind kind, std::span<const std::byte> payload,
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

    control_write_.encoded_size =
        static_cast<std::uint32_t>(encoded.bytes.size());
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
  MessageHandlerT message_handler_{};
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
  ReconnectTrigger reconnect_trigger_{ReconnectTrigger::kNone};
  int reconnect_errno_{0};
  bool awaiting_pong_{false};
  std::uint64_t last_ping_ns_{0};
#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
  std::unique_ptr<SocketTimestampingProbeState[]> socket_timestamping_probes_{};
  std::unique_ptr<std::uint32_t[]> socket_timestamping_free_slots_{};
  std::unique_ptr<std::uint32_t[]> socket_timestamping_active_slots_{};
  absl::flat_hash_map<std::uint64_t, std::uint32_t>
      socket_timestamping_sequence_to_slot_{};
  std::uint32_t socket_timestamping_probe_capacity_{0};
  std::uint32_t socket_timestamping_free_count_{0};
  std::uint32_t socket_timestamping_active_count_{0};
  std::uint64_t pending_socket_timestamping_sequence_{0};
  std::uint32_t pending_socket_timestamping_probe_slot_{
      kInvalidSocketTimestampingProbeSlot};
  std::uint32_t socket_timestamping_next_tx_id_{0};
  std::int64_t last_socket_rx_software_timestamp_ns_{0};
#endif
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_CRITICAL_SESSION_H_
