#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_LATENCY_DIAGNOSTICS_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_LATENCY_DIAGNOSTICS_H_

#include <cstddef>
#include <cstdint>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "core/common/order_ack_diagnostic_level.h"
#include "core/websocket/socket_diagnostics.h"
#include "core/websocket/socket_timestamping.h"
#include "core/websocket/types.h"

namespace aquila::gate {

enum class OrderLatencyDiagnosticReason : std::uint8_t {
  kAckRttThreshold,
  kSendToDriveReadThreshold,
  kDriveReadDurationThreshold,
  kDiagnosticTimeout,
};

struct OrderLatencyDiagnosticConfig {
  std::int64_t ack_rtt_threshold_ns{20'000'000};
  std::int64_t send_to_first_drive_read_threshold_ns{3'000'000};
  std::int64_t drive_read_duration_threshold_ns{1'000'000};
  std::int64_t diagnostic_window_timeout_ns{250'000'000};
  std::uint32_t max_logs_per_second{10};
};

struct OrderLatencyDiagnosticWindow {
  std::uint64_t local_order_id{0};
  std::uint64_t request_sequence{0};
  std::int64_t request_send_local_ns{0};
  std::size_t inflight_at_send{0};
  int owner_thread_tid{-1};
  websocket::WritePathDiagnostics write_path{};
  websocket::SocketSendQueueDiagnostics socket_send_queue{};
  websocket::SocketTimestampingSnapshot socket_timestamps{};
  websocket::SocketTimestampingStages socket_timestamp_stages{};
};

struct OrderLatencyDiagnosticLogRecord {
  OrderLatencyDiagnosticReason reason{
      OrderLatencyDiagnosticReason::kAckRttThreshold};
  std::uint64_t local_order_id{0};
  std::uint64_t request_sequence{0};
  std::int64_t request_send_local_ns{0};
  std::int64_t ack_local_receive_ns{0};
  std::int64_t ack_exchange_ns{0};
  std::int64_t ack_rtt_ns{0};
  std::int64_t send_to_first_after_hook_ns{0};
  std::int64_t send_to_first_drive_read_ns{0};
  std::int64_t drive_read_duration_ns{0};
  std::int64_t max_observed_drive_read_duration_ns{0};
  std::size_t inflight_at_send{0};
  std::int64_t max_runtime_loop_gap_ns{0};
  std::uint64_t runtime_loop_iterations_before_ack{0};
  int owner_thread_tid{-1};
  std::int64_t order_encode_done_ns{0};
  std::int64_t ws_frame_encode_done_ns{0};
  std::int64_t write_enqueue_ns{0};
  std::int64_t drive_write_enter_ns{0};
  std::int64_t write_some_enter_ns{0};
  std::int64_t write_some_return_ns{0};
  std::int64_t write_complete_ns{0};
  std::int64_t write_some_bytes{0};
  std::int64_t write_complete_bytes{0};
  int write_errno{0};
  bool write_eagain{false};
  std::size_t pending_write_count_after{0};
  bool socket_send_queue_available{false};
  std::uint32_t tcp_sendq_bytes{0};
  std::uint32_t tcp_notsent_bytes{0};
  websocket::SocketTimestampingSnapshot socket_timestamps{};
  websocket::SocketTimestampingStages socket_timestamp_stages{};
};

struct OrderLatencyDiagnosticAckResult {
  bool found{false};
  bool emitted{false};
  OrderLatencyDiagnosticLogRecord record{};
};

class OrderAckLatencyDiagnostics {
 public:
  explicit OrderAckLatencyDiagnostics(
      OrderLatencyDiagnosticConfig config = {}) noexcept
      : config_(config) {}

  void reserve(std::size_t capacity) {
    if constexpr (!core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      (void)capacity;
      return;
    }
    windows_.reserve(capacity);
  }

  void Arm(OrderLatencyDiagnosticWindow window) {
    if constexpr (!core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      (void)window;
      return;
    }
    if (window.request_sequence == 0 || window.request_send_local_ns <= 0) {
      return;
    }
    WindowState state{};
    state.local_order_id = window.local_order_id;
    state.request_sequence = window.request_sequence;
    state.request_send_local_ns = window.request_send_local_ns;
    state.inflight_at_send = window.inflight_at_send;
    state.owner_thread_tid = window.owner_thread_tid;
    state.write_path = window.write_path;
    state.socket_send_queue = window.socket_send_queue;
    state.socket_timestamps = window.socket_timestamps;
    state.socket_timestamp_stages = window.socket_timestamp_stages;
    windows_[window.request_sequence] = state;
  }

  void Erase(std::uint64_t request_sequence) noexcept {
    if constexpr (!core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      (void)request_sequence;
      return;
    }
    windows_.erase(request_sequence);
  }

  void clear() noexcept {
    windows_.clear();
  }

  [[nodiscard]] std::size_t active_count() const noexcept {
    if constexpr (!core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      return 0;
    }
    return windows_.size();
  }

  [[nodiscard]] bool empty() const noexcept {
    if constexpr (!core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      return true;
    }
    return windows_.empty();
  }

  template <typename Handler>
  std::size_t RecordAfterRuntimeHook(std::int64_t now_ns,
                                     Handler&& handler) noexcept {
    if constexpr (!core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      (void)now_ns;
      (void)handler;
      return 0;
    }
    if (now_ns <= 0) {
      return 0;
    }
    std::size_t emitted = 0;
    for (auto& [_, window] : windows_) {
      RecordRuntimeLoopIteration(now_ns, window);
      if (window.first_after_runtime_hook_ns == 0) {
        window.first_after_runtime_hook_ns = now_ns;
      }
      emitted += MaybeEmitTimeout(now_ns, window, handler);
    }
    return emitted;
  }

  template <typename Handler>
  std::size_t RecordBeforeDriveRead(std::int64_t now_ns,
                                    Handler&& handler) noexcept {
    if constexpr (!core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      (void)now_ns;
      (void)handler;
      return 0;
    }
    if (now_ns <= 0) {
      return 0;
    }
    std::size_t emitted = 0;
    for (auto& [_, window] : windows_) {
      window.last_before_drive_read_ns = now_ns;
      if (window.first_before_drive_read_ns == 0) {
        window.first_before_drive_read_ns = now_ns;
        const std::int64_t elapsed = now_ns - window.request_send_local_ns;
        if (!window.logged_send_to_drive_read &&
            elapsed > config_.send_to_first_drive_read_threshold_ns) {
          window.logged_send_to_drive_read = true;
          emitted += Emit(
              now_ns, OrderLatencyDiagnosticReason::kSendToDriveReadThreshold,
              window, handler);
        }
      }
      emitted += MaybeEmitTimeout(now_ns, window, handler);
    }
    return emitted;
  }

  template <typename Handler>
  std::size_t RecordAfterDriveRead(std::int64_t now_ns,
                                   Handler&& handler) noexcept {
    if constexpr (!core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      (void)now_ns;
      (void)handler;
      return 0;
    }
    if (now_ns <= 0) {
      return 0;
    }
    std::size_t emitted = 0;
    for (auto& [_, window] : windows_) {
      if (window.last_before_drive_read_ns > 0) {
        const std::int64_t duration = now_ns - window.last_before_drive_read_ns;
        UpdateDriveReadDuration(window, duration);
        if (!window.logged_drive_read_duration &&
            duration > config_.drive_read_duration_threshold_ns) {
          window.logged_drive_read_duration = true;
          emitted += Emit(
              now_ns, OrderLatencyDiagnosticReason::kDriveReadDurationThreshold,
              window, handler);
        }
      }
      emitted += MaybeEmitTimeout(now_ns, window, handler);
    }
    return emitted;
  }

  template <typename Handler>
  bool RecordAck(std::uint64_t request_sequence,
                 std::int64_t ack_local_receive_ns,
                 std::int64_t ack_exchange_ns, Handler&& handler) noexcept {
    return RecordAck(request_sequence, ack_local_receive_ns, ack_exchange_ns, 0,
                     websocket::SocketTimestampingSnapshot{}, handler);
  }

  template <typename Handler>
  bool RecordAck(std::uint64_t request_sequence,
                 std::int64_t ack_local_receive_ns,
                 std::int64_t ack_exchange_ns,
                 std::int64_t current_drive_read_start_ns,
                 Handler&& handler) noexcept {
    return RecordAck(request_sequence, ack_local_receive_ns, ack_exchange_ns,
                     current_drive_read_start_ns,
                     websocket::SocketTimestampingSnapshot{}, handler);
  }

  template <typename Handler>
  bool RecordAck(std::uint64_t request_sequence,
                 std::int64_t ack_local_receive_ns,
                 std::int64_t ack_exchange_ns,
                 const websocket::SocketTimestampingSnapshot& socket_timestamps,
                 Handler&& handler) noexcept {
    return RecordAck(request_sequence, ack_local_receive_ns, ack_exchange_ns, 0,
                     socket_timestamps, handler);
  }

  template <typename Handler>
  bool RecordAck(std::uint64_t request_sequence,
                 std::int64_t ack_local_receive_ns,
                 std::int64_t ack_exchange_ns,
                 std::int64_t current_drive_read_start_ns,
                 const websocket::SocketTimestampingSnapshot& socket_timestamps,
                 Handler&& handler) noexcept {
    return RecordAckWithRecord(request_sequence, ack_local_receive_ns,
                               ack_exchange_ns, current_drive_read_start_ns,
                               socket_timestamps,
                               std::forward<Handler>(handler))
        .emitted;
  }

  template <typename Handler>
  OrderLatencyDiagnosticAckResult RecordAckWithRecord(
      std::uint64_t request_sequence, std::int64_t ack_local_receive_ns,
      std::int64_t ack_exchange_ns, std::int64_t current_drive_read_start_ns,
      const websocket::SocketTimestampingSnapshot& socket_timestamps,
      Handler&& handler) noexcept {
    if constexpr (!core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      (void)request_sequence;
      (void)ack_local_receive_ns;
      (void)ack_exchange_ns;
      (void)current_drive_read_start_ns;
      (void)socket_timestamps;
      (void)handler;
      return {};
    }
    auto it = windows_.find(request_sequence);
    if (it == windows_.end()) {
      return {};
    }
    WindowState window = it->second;
    window.ack_local_receive_ns = ack_local_receive_ns;
    window.ack_exchange_ns = ack_exchange_ns;
    window.socket_timestamps = socket_timestamps;
    window.socket_timestamp_stages =
        websocket::ComputeSocketTimestampingStages(window.socket_timestamps);
    bool should_emit_drive_read_duration = false;
    if (current_drive_read_start_ns > 0 &&
        ack_local_receive_ns > current_drive_read_start_ns) {
      const std::int64_t duration =
          ack_local_receive_ns - current_drive_read_start_ns;
      UpdateDriveReadDuration(window, duration);
      should_emit_drive_read_duration =
          !window.logged_drive_read_duration &&
          duration > config_.drive_read_duration_threshold_ns;
      if (should_emit_drive_read_duration) {
        window.logged_drive_read_duration = true;
      }
    }
    const std::int64_t ack_rtt_ns =
        ack_local_receive_ns - window.request_send_local_ns;
    const bool ack_rtt_exceeds_threshold =
        ack_rtt_ns > config_.ack_rtt_threshold_ns;
    const bool should_capture_record =
        ack_rtt_exceeds_threshold || should_emit_drive_read_duration;
    OrderLatencyDiagnosticAckResult result{
        .found = should_capture_record,
        .emitted = false,
        .record =
            should_capture_record
                ? MakeRecord(OrderLatencyDiagnosticReason::kAckRttThreshold,
                             window)
                : OrderLatencyDiagnosticLogRecord{},
    };
    const bool should_log =
        ack_rtt_exceeds_threshold && AllowLog(ack_local_receive_ns);
    if (should_log) {
      handler(result.record);
      result.emitted = true;
    } else if (should_emit_drive_read_duration) {
      should_emit_drive_read_duration =
          Emit(ack_local_receive_ns,
               OrderLatencyDiagnosticReason::kDriveReadDurationThreshold,
               window, handler) != 0;
      result.emitted = should_emit_drive_read_duration;
    }
    windows_.erase(it);
    return result;
  }

 private:
  struct WindowState {
    std::uint64_t local_order_id{0};
    std::uint64_t request_sequence{0};
    std::int64_t request_send_local_ns{0};
    std::int64_t first_after_runtime_hook_ns{0};
    std::int64_t first_before_drive_read_ns{0};
    std::int64_t last_before_drive_read_ns{0};
    std::int64_t last_drive_read_duration_ns{0};
    std::int64_t max_observed_drive_read_duration_ns{0};
    std::int64_t ack_local_receive_ns{0};
    std::int64_t ack_exchange_ns{0};
    std::size_t inflight_at_send{0};
    std::int64_t last_runtime_loop_iteration_ns{0};
    std::int64_t max_runtime_loop_gap_ns{0};
    std::uint64_t runtime_loop_iterations_before_ack{0};
    int owner_thread_tid{-1};
    websocket::WritePathDiagnostics write_path{};
    websocket::SocketSendQueueDiagnostics socket_send_queue{};
    websocket::SocketTimestampingSnapshot socket_timestamps{};
    websocket::SocketTimestampingStages socket_timestamp_stages{};
    bool logged_send_to_drive_read{false};
    bool logged_drive_read_duration{false};
    bool logged_timeout{false};
  };

  template <typename Handler>
  std::size_t MaybeEmitTimeout(std::int64_t now_ns, WindowState& window,
                               Handler&& handler) noexcept {
    if (window.logged_timeout || now_ns - window.request_send_local_ns <=
                                     config_.diagnostic_window_timeout_ns) {
      return 0;
    }
    window.logged_timeout = true;
    return Emit(now_ns, OrderLatencyDiagnosticReason::kDiagnosticTimeout,
                window, handler);
  }

  static void UpdateDriveReadDuration(WindowState& window,
                                      std::int64_t duration_ns) noexcept {
    window.last_drive_read_duration_ns = duration_ns;
    if (duration_ns > window.max_observed_drive_read_duration_ns) {
      window.max_observed_drive_read_duration_ns = duration_ns;
    }
  }

  static void RecordRuntimeLoopIteration(std::int64_t now_ns,
                                         WindowState& window) noexcept {
    if (window.last_runtime_loop_iteration_ns > 0 &&
        now_ns > window.last_runtime_loop_iteration_ns) {
      const std::int64_t gap_ns =
          now_ns - window.last_runtime_loop_iteration_ns;
      if (gap_ns > window.max_runtime_loop_gap_ns) {
        window.max_runtime_loop_gap_ns = gap_ns;
      }
    }
    window.last_runtime_loop_iteration_ns = now_ns;
    ++window.runtime_loop_iterations_before_ack;
  }

  template <typename Handler>
  std::size_t Emit(std::int64_t now_ns, OrderLatencyDiagnosticReason reason,
                   const WindowState& window, Handler&& handler) noexcept {
    if (!AllowLog(now_ns)) {
      return 0;
    }
    handler(MakeRecord(reason, window));
    return 1;
  }

  [[nodiscard]] OrderLatencyDiagnosticLogRecord MakeRecord(
      OrderLatencyDiagnosticReason reason,
      const WindowState& window) const noexcept {
    return OrderLatencyDiagnosticLogRecord{
        .reason = reason,
        .local_order_id = window.local_order_id,
        .request_sequence = window.request_sequence,
        .request_send_local_ns = window.request_send_local_ns,
        .ack_local_receive_ns = window.ack_local_receive_ns,
        .ack_exchange_ns = window.ack_exchange_ns,
        .ack_rtt_ns =
            window.ack_local_receive_ns > 0
                ? window.ack_local_receive_ns - window.request_send_local_ns
                : 0,
        .send_to_first_after_hook_ns =
            window.first_after_runtime_hook_ns > 0
                ? window.first_after_runtime_hook_ns -
                      window.request_send_local_ns
                : 0,
        .send_to_first_drive_read_ns = window.first_before_drive_read_ns > 0
                                           ? window.first_before_drive_read_ns -
                                                 window.request_send_local_ns
                                           : 0,
        .drive_read_duration_ns = window.last_drive_read_duration_ns,
        .max_observed_drive_read_duration_ns =
            window.max_observed_drive_read_duration_ns,
        .inflight_at_send = window.inflight_at_send,
        .max_runtime_loop_gap_ns = window.max_runtime_loop_gap_ns,
        .runtime_loop_iterations_before_ack =
            window.runtime_loop_iterations_before_ack,
        .owner_thread_tid = window.owner_thread_tid,
        .order_encode_done_ns = window.write_path.order_encode_done_ns,
        .ws_frame_encode_done_ns = window.write_path.ws_frame_encode_done_ns,
        .write_enqueue_ns = window.write_path.write_enqueue_ns,
        .drive_write_enter_ns = window.write_path.drive_write_enter_ns,
        .write_some_enter_ns = window.write_path.write_some_enter_ns,
        .write_some_return_ns = window.write_path.write_some_return_ns,
        .write_complete_ns = window.write_path.write_complete_ns,
        .write_some_bytes = window.write_path.write_some_bytes,
        .write_complete_bytes = window.write_path.write_complete_bytes,
        .write_errno = window.write_path.write_errno,
        .write_eagain = window.write_path.write_eagain,
        .pending_write_count_after =
            window.write_path.pending_write_count_after,
        .socket_send_queue_available = window.socket_send_queue.available,
        .tcp_sendq_bytes = window.socket_send_queue.sendq_bytes,
        .tcp_notsent_bytes = window.socket_send_queue.notsent_bytes,
        .socket_timestamps = window.socket_timestamps,
        .socket_timestamp_stages = window.socket_timestamp_stages,
    };
  }

  [[nodiscard]] bool AllowLog(std::int64_t now_ns) noexcept {
    if (config_.max_logs_per_second == 0) {
      return false;
    }
    constexpr std::int64_t kOneSecondNs = 1'000'000'000;
    if (rate_limit_window_start_ns_ == 0 ||
        now_ns - rate_limit_window_start_ns_ >= kOneSecondNs) {
      rate_limit_window_start_ns_ = now_ns;
      logs_in_rate_limit_window_ = 0;
    }
    if (logs_in_rate_limit_window_ >= config_.max_logs_per_second) {
      return false;
    }
    ++logs_in_rate_limit_window_;
    return true;
  }

  OrderLatencyDiagnosticConfig config_{};
  absl::flat_hash_map<std::uint64_t, WindowState> windows_;
  std::int64_t rate_limit_window_start_ns_{0};
  std::uint32_t logs_in_rate_limit_window_{0};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_LATENCY_DIAGNOSTICS_H_
