#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_LATENCY_DIAGNOSTICS_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_LATENCY_DIAGNOSTICS_H_

#include <cstddef>
#include <cstdint>
#include <utility>

#include <absl/container/flat_hash_map.h>

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
};

class OrderAckLatencyDiagnostics {
 public:
  explicit OrderAckLatencyDiagnostics(
      OrderLatencyDiagnosticConfig config = {}) noexcept
      : config_(config) {}

  void reserve(std::size_t capacity) {
    windows_.reserve(capacity);
  }

  void Arm(OrderLatencyDiagnosticWindow window) {
    if (window.request_sequence == 0 || window.request_send_local_ns <= 0) {
      return;
    }
    WindowState state{};
    state.local_order_id = window.local_order_id;
    state.request_sequence = window.request_sequence;
    state.request_send_local_ns = window.request_send_local_ns;
    state.inflight_at_send = window.inflight_at_send;
    windows_[window.request_sequence] = state;
  }

  void Erase(std::uint64_t request_sequence) noexcept {
    windows_.erase(request_sequence);
  }

  void clear() noexcept {
    windows_.clear();
  }

  [[nodiscard]] std::size_t active_count() const noexcept {
    return windows_.size();
  }

  [[nodiscard]] bool empty() const noexcept {
    return windows_.empty();
  }

  template <typename Handler>
  std::size_t RecordAfterRuntimeHook(std::int64_t now_ns,
                                     Handler&& handler) noexcept {
    if (now_ns <= 0) {
      return 0;
    }
    std::size_t emitted = 0;
    for (auto& [_, window] : windows_) {
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
    if (now_ns <= 0) {
      return 0;
    }
    std::size_t emitted = 0;
    for (auto& [_, window] : windows_) {
      if (window.last_before_drive_read_ns > 0) {
        const std::int64_t duration = now_ns - window.last_before_drive_read_ns;
        if (duration > window.max_observed_drive_read_duration_ns) {
          window.max_observed_drive_read_duration_ns = duration;
        }
        window.last_drive_read_duration_ns = duration;
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
    auto it = windows_.find(request_sequence);
    if (it == windows_.end()) {
      return false;
    }
    WindowState window = it->second;
    window.ack_local_receive_ns = ack_local_receive_ns;
    window.ack_exchange_ns = ack_exchange_ns;
    const std::int64_t ack_rtt_ns =
        ack_local_receive_ns - window.request_send_local_ns;
    const bool should_log = ack_rtt_ns > config_.ack_rtt_threshold_ns &&
                            AllowLog(ack_local_receive_ns);
    if (should_log) {
      handler(
          MakeRecord(OrderLatencyDiagnosticReason::kAckRttThreshold, window));
    }
    windows_.erase(it);
    return should_log;
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
