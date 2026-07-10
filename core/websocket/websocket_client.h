#ifndef AQUILA_CORE_WEBSOCKET_WEBSOCKET_CLIENT_H_
#define AQUILA_CORE_WEBSOCKET_WEBSOCKET_CLIENT_H_

#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

#include "core/websocket/active_spin_loop.h"
#include "core/websocket/cold_path_loop.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/degraded_evaluator.h"
#include "core/websocket/metrics.h"
#include "core/websocket/plain_socket.h"
#include "core/websocket/reconnect_classifier.h"
#include "core/websocket/runtime_policy.h"
#include "core/websocket/state_machine.h"
#include "core/websocket/tls_socket.h"

namespace aquila::websocket {

using StateHandler = void (*)(void* context, ConnectionPhase phase) noexcept;
using ErrorHandler = void (*)(void* context, ConnectionError error) noexcept;
using RuntimeHook = void (*)(void* context) noexcept;
enum class RuntimeLoopProbePoint : std::uint8_t {
  kAfterRuntimeHook,
  kBeforeDriveRead,
  kAfterDriveRead,
};
using RuntimeLoopProbe = void (*)(void* context,
                                  RuntimeLoopProbePoint point) noexcept;

template <typename TransportSocketT, typename MessageHandlerT = MessageCallback>
class BasicWebSocketClient {
 public:
  static constexpr bool TransportUsesTls = TransportSocketT::kUsesTls;

  BasicWebSocketClient(ConnectionConfig config,
                       MessageHandlerT message_handler) noexcept
      : config_(std::move(config)),
        message_handler_(message_handler),
        prepared_write_arena_(config_.prepared_write_slots,
                              config_.prepared_write_bytes),
        core_(config_, transport_socket_, prepared_write_arena_, metrics_),
        spin_loop_(config_.runtime_policy),
        backoff_rng_(SeedBackoffRng()) {
    config_.enable_tls = TransportSocketT::kUsesTls;
    wakeup_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    cold_path_loop_.SetInterruptFd(wakeup_fd_);
  }

  ~BasicWebSocketClient() noexcept {
    if (wakeup_fd_ >= 0) {
      ::close(wakeup_fd_);
    }
  }

  bool PrepareRuntimeOnly() noexcept {
    RuntimePolicy effective_policy = config_.runtime_policy;
    if (effective_policy.affinity_mode == AffinityMode::kRequired &&
        effective_policy.io_cpu_id < 0) {
      effective_policy.affinity_mode = AffinityMode::kNone;
    }

    prepare_error_ = ConnectionError::kNone;
    if (!EnsureWakeupFd()) {
      prepare_error_ = ConnectionError::kSocketError;
      return false;
    }
    cold_path_loop_.SetInterruptFd(wakeup_fd_);
    if (!ApplyRuntimePolicy(effective_policy)) {
      prepare_error_ = ConnectionError::kSocketError;
      return false;
    }
    if (!transport_socket_.Init()) {
      prepare_error_ = config_.enable_tls ? ConnectionError::kTlsFailure
                                          : ConnectionError::kSocketError;
      return false;
    }
    runtime_prepared_ = true;
    core_.SetMessageHandler(message_handler_);
    return true;
  }

  void SetStateHandler(void* context, StateHandler handler) noexcept {
    state_context_ = context;
    state_handler_ = handler;
  }

  // Wrapper sessions use this hook for protocol work that must run before
  // external state handlers.
  void SetStateHook(void* context, StateHandler handler) noexcept {
    state_hook_context_ = context;
    state_hook_handler_ = handler;
  }

  void SetErrorHandler(void* context, ErrorHandler handler) noexcept {
    error_context_ = context;
    error_handler_ = handler;
  }

  void SetRuntimeHook(void* context, RuntimeHook handler) noexcept {
    runtime_hook_context_ = context;
    runtime_hook_handler_ = handler;
  }

  void SetRuntimeLoopProbe(void* context, RuntimeLoopProbe handler) noexcept {
    runtime_loop_probe_context_ = context;
    runtime_loop_probe_handler_ = handler;
  }

  [[nodiscard]] ConnectionPhase phase() const noexcept {
    return static_cast<ConnectionPhase>(
        observable_phase_.load(std::memory_order_acquire));
  }

  [[nodiscard]] ConnectionError last_error() const noexcept {
    return static_cast<ConnectionError>(
        observable_error_.load(std::memory_order_acquire));
  }

  [[nodiscard]] ReconnectTrigger last_reconnect_trigger() const noexcept {
    return static_cast<ReconnectTrigger>(
        observable_reconnect_trigger_.load(std::memory_order_acquire));
  }

  [[nodiscard]] int last_reconnect_errno() const noexcept {
    return observable_reconnect_errno_.load(std::memory_order_acquire);
  }

  bool Start() noexcept {
    if (!runtime_prepared_ && !PrepareRuntimeOnly()) {
      state_machine_.Fail(prepare_error_, ConnectionPhase::kDisconnected);
      NotifyError(state_machine_.last_error());
      NotifyState(state_machine_.phase());
      return false;
    }

    stop_requested_.store(false, std::memory_order_relaxed);
    DrainWakeupFd();

    std::uint32_t transient_failures = 0;
    bool reconnect_in_progress = false;
    while (!stop_requested_.load(std::memory_order_relaxed)) {
      cold_path_loop_.SetInterruptFd(wakeup_fd_);
      if (!cold_path_loop_.RunUntilActive(transport_socket_, state_machine_,
                                          config_, handshake_storage_)) {
        if (stop_requested_.load(std::memory_order_relaxed) ||
            state_machine_.phase() == ConnectionPhase::kClosing) {
          NotifyState(ConnectionPhase::kClosing);
          transport_socket_.Close();
          return true;
        }
        if (!HandleReconnectFailure(
                state_machine_.last_error(), ReconnectTrigger::kNone, 0,
                transient_failures, reconnect_in_progress)) {
          return false;
        }
        continue;
      }

      if (reconnect_in_progress) {
        ++metrics_.reconnects;
        reconnect_in_progress = false;
        transient_failures = 0;
      }

      NotifyState(state_machine_.phase());
      const auto handshake_leftover =
          cold_path_loop_.HandshakeLeftover(std::span<const char>(
              handshake_storage_.data(), handshake_storage_.size()));
      if (!handshake_leftover.empty()) {
        core_.FeedReadBytes(handshake_leftover);
        if (core_.ShouldReconnect()) {
          MarkDegradedInactive();
          if (!HandleReconnectFailure(
                  core_.LastError(), core_.LastReconnectTrigger(),
                  core_.LastReconnectErrno(), transient_failures,
                  reconnect_in_progress)) {
            return false;
          }
          continue;
        }
      }
      RuntimeSession runtime_session{
          core_,
          metrics_,
          stop_requested_,
          degraded_evaluator_,
          DegradedEvaluationInterval(),
          this,
      };
      spin_loop_.Run(runtime_session);
      transport_socket_.Close();
      if (stop_requested_.load(std::memory_order_relaxed)) {
        MarkDegradedInactive();
        state_machine_.Enter(ConnectionPhase::kClosed);
        NotifyState(state_machine_.phase());
        return true;
      }
      if (core_.ShouldReconnect()) {
        MarkDegradedInactive();
        if (!HandleReconnectFailure(
                core_.LastError(), core_.LastReconnectTrigger(),
                core_.LastReconnectErrno(), transient_failures,
                reconnect_in_progress)) {
          return false;
        }
        continue;
      }
      MarkDegradedInactive();
      state_machine_.Enter(ConnectionPhase::kClosed);
      NotifyState(state_machine_.phase());
      return true;
    }

    transport_socket_.Close();
    MarkDegradedInactive();
    state_machine_.Enter(ConnectionPhase::kClosed);
    NotifyState(state_machine_.phase());
    return true;
  }

  CriticalSession<TransportSocketT, MessageHandlerT>& Core() noexcept {
    return core_;
  }

  void Stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
    SignalWakeup();
  }

  void Wakeup() noexcept {
    SignalWakeup();
  }

  Metrics SnapshotMetrics() const noexcept {
    return metrics_;
  }

  void RequestReconnect(ConnectionError error, ReconnectTrigger trigger,
                        int error_number = 0) noexcept {
    core_.RequestReconnect(error, trigger, error_number);
  }

 private:
  struct RuntimeSession {
    CriticalSession<TransportSocketT, MessageHandlerT>& core;
    Metrics& metrics;
    std::atomic<bool>& stop_requested;
    DegradedEvaluator& degraded_evaluator;
    std::uint32_t evaluation_interval_iterations;
    BasicWebSocketClient* client;
    std::uint32_t iterations_since_evaluation{0};

    void DriveWrite() noexcept {
      core.DriveWrite();
    }

    void BeforeDrive() noexcept {
      client->RunRuntimeHook();
      client->NotifyRuntimeLoopProbe(RuntimeLoopProbePoint::kAfterRuntimeHook);
    }

    void DriveRead() noexcept {
      core.DriveRead();
    }

    void BeforeDriveRead() noexcept {
      client->NotifyRuntimeLoopProbe(RuntimeLoopProbePoint::kBeforeDriveRead);
    }

    void AfterDriveRead() noexcept {
      client->NotifyRuntimeLoopProbe(RuntimeLoopProbePoint::kAfterDriveRead);
    }

    void AdvanceHeartbeat(std::uint64_t now_ns) noexcept {
      core.AdvanceHeartbeat(now_ns);
    }

    std::uint32_t ClockCheckInterval(
        std::uint32_t default_interval) const noexcept {
      if (evaluation_interval_iterations == 0) {
        return default_interval;
      }
      return default_interval < evaluation_interval_iterations
                 ? default_interval
                 : evaluation_interval_iterations;
    }

    void AdvanceClock(std::uint64_t now_ns,
                      std::uint32_t elapsed_iterations) noexcept {
      core.AdvanceHeartbeat(now_ns);
      EvaluateDegradedIfDue(now_ns, elapsed_iterations);
    }

    bool ShouldReconnect() const noexcept {
      return stop_requested.load(std::memory_order_relaxed) ||
             core.ShouldReconnect();
    }

    void EvaluateDegradedIfDue(std::uint64_t now_ns,
                               std::uint32_t elapsed_iterations) noexcept {
      iterations_since_evaluation += elapsed_iterations;
      if (iterations_since_evaluation < evaluation_interval_iterations) {
        return;
      }
      iterations_since_evaluation = 0;
      const auto evaluation = degraded_evaluator.Evaluate(DegradedSample{
          .now_ns = now_ns,
          .pending_write_count =
              static_cast<std::uint64_t>(core.PendingWriteCount()),
          .prepared_write_slots =
              static_cast<std::uint64_t>(core.PendingWriteCapacity()),
          .consumer_backpressure_drops = metrics.consumer_backpressure_drops,
          .frame_codec_capacity_exhaustions =
              metrics.frame_codec_capacity_exhaustions,
          .awaiting_pong = core.AwaitingPong(),
          .last_ping_ns = core.LastPingNs(),
      });
      if (evaluation.entered) {
        metrics.degraded_active = 1;
        ++metrics.degraded_enter_count;
        NotifyState(ConnectionPhase::kDegraded);
      }
      if (evaluation.exited) {
        metrics.degraded_active = 0;
        ++metrics.degraded_exit_count;
        NotifyState(ConnectionPhase::kActive);
      }
    }

    void NotifyState(ConnectionPhase phase) noexcept {
      client->NotifyState(phase);
    }
  };

  static std::uint64_t SeedBackoffRng() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    std::uint64_t seed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    pthread_t self = pthread_self();
    std::uint64_t thread_bits = 0;
    static_assert(sizeof(thread_bits) >= sizeof(self));
    std::memcpy(&thread_bits, &self, sizeof(self));
    seed ^= thread_bits;
    return seed == 0 ? 0x9e37'79b9'7f4a'7c15ULL : seed;
  }

  bool EnsureWakeupFd() noexcept {
    if (wakeup_fd_ >= 0) {
      return true;
    }
    wakeup_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    return wakeup_fd_ >= 0;
  }

  void SignalWakeup() noexcept {
    if (wakeup_fd_ < 0) {
      return;
    }
    const std::uint64_t one = 1;
    const ssize_t written = ::write(wakeup_fd_, &one, sizeof(one));
    (void)written;
  }

  void DrainWakeupFd() noexcept {
    if (wakeup_fd_ < 0) {
      return;
    }
    std::uint64_t value = 0;
    while (::read(wakeup_fd_, &value, sizeof(value)) ==
           static_cast<ssize_t>(sizeof(value))) {
    }
  }

  bool SleepForBackoff(std::uint32_t backoff_ms) noexcept {
    if (backoff_ms == 0) {
      return !stop_requested_.load(std::memory_order_relaxed);
    }
    if (wakeup_fd_ < 0) {
      return false;
    }

    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
      return false;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = wakeup_fd_;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wakeup_fd_, &event) != 0) {
      ::close(epoll_fd);
      return false;
    }

    epoll_event ready{};
    while (true) {
      const int ready_count =
          epoll_wait(epoll_fd, &ready, 1, static_cast<int>(backoff_ms));
      if (ready_count > 0) {
        DrainWakeupFd();
        ::close(epoll_fd);
        return false;
      }
      if (ready_count == 0) {
        ::close(epoll_fd);
        return !stop_requested_.load(std::memory_order_relaxed);
      }
      if (errno == EINTR) {
        continue;
      }
      ::close(epoll_fd);
      return false;
    }
  }

  bool HandleReconnectFailure(ConnectionError error,
                              ReconnectTrigger reconnect_trigger,
                              int reconnect_errno,
                              std::uint32_t& transient_failures,
                              bool& reconnect_in_progress) noexcept {
    transport_socket_.Close();
    StoreReconnectDiagnostics(reconnect_trigger, reconnect_errno);
    if (!config_.reconnect.enabled || Classify(error) == FailureClass::kFatal) {
      state_machine_.Fail(error, ConnectionPhase::kClosed);
      NotifyError(state_machine_.last_error());
      NotifyState(state_machine_.phase());
      return false;
    }

    ++transient_failures;
    if (config_.reconnect.max_attempts != 0 &&
        transient_failures >= config_.reconnect.max_attempts) {
      state_machine_.Fail(error, ConnectionPhase::kClosed);
      NotifyError(state_machine_.last_error());
      NotifyState(state_machine_.phase());
      return false;
    }

    state_machine_.Fail(error, ConnectionPhase::kReconnectBackoff);
    NotifyError(state_machine_.last_error());
    NotifyState(state_machine_.phase());
    const std::uint32_t backoff_ms = ComputeBackoffMs(
        transient_failures - 1, config_.reconnect, backoff_rng_);
    if (!SleepForBackoff(backoff_ms)) {
      if (stop_requested_.load(std::memory_order_relaxed)) {
        state_machine_.Enter(ConnectionPhase::kClosing);
        NotifyState(state_machine_.phase());
        return true;
      }
      StoreReconnectDiagnostics(ReconnectTrigger::kNone, 0);
      state_machine_.Fail(ConnectionError::kSocketError,
                          ConnectionPhase::kClosed);
      NotifyError(state_machine_.last_error());
      NotifyState(state_machine_.phase());
      return false;
    }

    core_.Reset();
    degraded_evaluator_.Reset();
    reconnect_in_progress = true;
    return true;
  }

  void NotifyState(ConnectionPhase phase) noexcept {
    observable_phase_.store(static_cast<std::uint8_t>(phase),
                            std::memory_order_release);
    if (phase == ConnectionPhase::kActive) {
      observable_error_.store(static_cast<std::uint8_t>(ConnectionError::kNone),
                              std::memory_order_release);
      StoreReconnectDiagnostics(ReconnectTrigger::kNone, 0);
    }
    if (state_hook_handler_ != nullptr) {
      state_hook_handler_(state_hook_context_, phase);
    }
    if (state_handler_ != nullptr) {
      state_handler_(state_context_, phase);
    }
  }

  std::uint32_t DegradedEvaluationInterval() const noexcept {
    if (config_.degraded.evaluation_interval_iterations != 0) {
      return config_.degraded.evaluation_interval_iterations;
    }
    if (config_.runtime_policy.spin_iterations_before_clock_check != 0) {
      return config_.runtime_policy.spin_iterations_before_clock_check;
    }
    return 1;
  }

  void MarkDegradedInactive() noexcept {
    if (metrics_.degraded_active != 0) {
      metrics_.degraded_active = 0;
      ++metrics_.degraded_exit_count;
      degraded_evaluator_.Reset();
    }
  }

  void NotifyError(ConnectionError error) noexcept {
    observable_error_.store(static_cast<std::uint8_t>(error),
                            std::memory_order_release);
    if (error_handler_ != nullptr) {
      error_handler_(error_context_, error);
    }
  }

  void StoreReconnectDiagnostics(ReconnectTrigger reconnect_trigger,
                                 int reconnect_errno) noexcept {
    observable_reconnect_trigger_.store(
        static_cast<std::uint8_t>(reconnect_trigger),
        std::memory_order_release);
    observable_reconnect_errno_.store(reconnect_errno,
                                      std::memory_order_release);
  }

  void RunRuntimeHook() noexcept {
    if (runtime_hook_handler_ != nullptr) {
      runtime_hook_handler_(runtime_hook_context_);
    }
  }

  void NotifyRuntimeLoopProbe(RuntimeLoopProbePoint point) noexcept {
    if (runtime_loop_probe_handler_ != nullptr) {
      runtime_loop_probe_handler_(runtime_loop_probe_context_, point);
    }
  }

  ConnectionConfig config_{};
  MessageHandlerT message_handler_{};
  Metrics metrics_{};
  TransportSocketT transport_socket_{};
  PreparedWriteArena prepared_write_arena_;
  CriticalSession<TransportSocketT, MessageHandlerT> core_;
  StateMachine state_machine_{};
  ColdPathLoop cold_path_loop_{};
  ActiveSpinLoop spin_loop_;
  BackoffRng backoff_rng_;
  DegradedEvaluator degraded_evaluator_{config_.degraded};
  std::array<char, 4096> handshake_storage_{};
  std::atomic<bool> stop_requested_{false};
  int wakeup_fd_{-1};
  bool runtime_prepared_{false};
  ConnectionError prepare_error_{ConnectionError::kNone};
  std::atomic<std::uint8_t> observable_phase_{
      static_cast<std::uint8_t>(ConnectionPhase::kDisconnected)};
  std::atomic<std::uint8_t> observable_error_{
      static_cast<std::uint8_t>(ConnectionError::kNone)};
  std::atomic<std::uint8_t> observable_reconnect_trigger_{
      static_cast<std::uint8_t>(ReconnectTrigger::kNone)};
  std::atomic<int> observable_reconnect_errno_{0};
  void* state_hook_context_{nullptr};
  StateHandler state_hook_handler_{nullptr};
  void* state_context_{nullptr};
  StateHandler state_handler_{nullptr};
  void* error_context_{nullptr};
  ErrorHandler error_handler_{nullptr};
  void* runtime_hook_context_{nullptr};
  RuntimeHook runtime_hook_handler_{nullptr};
  void* runtime_loop_probe_context_{nullptr};
  RuntimeLoopProbe runtime_loop_probe_handler_{nullptr};
};

template <typename MessageHandlerT>
using WebSocketClientWithHandler =
    BasicWebSocketClient<TlsSocket, MessageHandlerT>;

template <typename MessageHandlerT>
using PlainWebSocketClientWithHandler =
    BasicWebSocketClient<PlainSocket, MessageHandlerT>;

using WebSocketClient = WebSocketClientWithHandler<MessageCallback>;
using PlainWebSocketClient = PlainWebSocketClientWithHandler<MessageCallback>;

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_WEBSOCKET_CLIENT_H_
