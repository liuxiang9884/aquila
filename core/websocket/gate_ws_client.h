#ifndef AQUILA_CORE_WEBSOCKET_GATE_WS_CLIENT_H_
#define AQUILA_CORE_WEBSOCKET_GATE_WS_CLIENT_H_

#include <array>
#include <atomic>
#include <utility>

#include "core/websocket/active_spin_loop.h"
#include "core/websocket/cold_path_loop.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/metrics.h"
#include "core/websocket/runtime_policy.h"
#include "core/websocket/state_machine.h"
#include "core/websocket/tls_socket.h"

namespace aquila::websocket {

using StateHandler = void (*)(void* context, ConnectionPhase phase) noexcept;
using ErrorHandler = void (*)(void* context, ConnectionError error) noexcept;

class GateWsClient {
 public:
  GateWsClient(ConnectionConfig config, MessageConsumer consumer) noexcept
      : config_(std::move(config)),
        consumer_(consumer),
        prepared_write_arena_(config_.prepared_write_slots,
                              config_.prepared_write_bytes),
        core_(config_, tls_socket_, prepared_write_arena_, metrics_),
        spin_loop_(config_.runtime_policy) {}

  bool PrepareRuntimeOnly() noexcept {
    RuntimePolicy effective_policy = config_.runtime_policy;
    if (effective_policy.affinity_mode == AffinityMode::kRequired &&
        effective_policy.io_cpu_id < 0) {
      effective_policy.affinity_mode = AffinityMode::kNone;
    }

    prepare_error_ = ConnectionError::kNone;
    if (!ApplyRuntimePolicy(effective_policy)) {
      prepare_error_ = ConnectionError::kSocketError;
      return false;
    }
    if (!tls_socket_.Init()) {
      prepare_error_ =
          config_.enable_tls ? ConnectionError::kTlsFailure
                             : ConnectionError::kSocketError;
      return false;
    }
    runtime_prepared_ = true;
    core_.SetConsumer(consumer_);
    return true;
  }

  void SetStateHandler(void* context, StateHandler handler) noexcept {
    state_context_ = context;
    state_handler_ = handler;
  }

  void SetErrorHandler(void* context, ErrorHandler handler) noexcept {
    error_context_ = context;
    error_handler_ = handler;
  }

  bool Start() noexcept {
    if (!runtime_prepared_ && !PrepareRuntimeOnly()) {
      state_machine_.Fail(prepare_error_, ConnectionPhase::kDisconnected);
      NotifyError(state_machine_.last_error());
      NotifyState(state_machine_.phase());
      return false;
    }

    stop_requested_.store(false);
    if (!cold_path_loop_.RunUntilActive(tls_socket_, state_machine_, config_,
                                        handshake_storage_)) {
      NotifyError(state_machine_.last_error());
      NotifyState(state_machine_.phase());
      return false;
    }

    NotifyState(state_machine_.phase());
    RuntimeSession runtime_session{core_, stop_requested_};
    spin_loop_.Run(runtime_session);
    if (stop_requested_.load()) {
      state_machine_.Enter(ConnectionPhase::kClosed);
      NotifyState(state_machine_.phase());
      return true;
    }
    if (core_.ShouldReconnect()) {
      state_machine_.Fail(core_.LastError(), ConnectionPhase::kClosed);
      NotifyError(state_machine_.last_error());
      NotifyState(state_machine_.phase());
      return false;
    }
    state_machine_.Enter(ConnectionPhase::kClosed);
    NotifyState(state_machine_.phase());
    return true;
  }

  CriticalSession<TlsSocket>& Core() noexcept { return core_; }

  void Stop() noexcept {
    stop_requested_.store(true);
    tls_socket_.Close();
  }

  Metrics SnapshotMetrics() const noexcept { return metrics_; }

 private:
  struct RuntimeSession {
    CriticalSession<TlsSocket>& core;
    std::atomic<bool>& stop_requested;

    void DriveWrite() noexcept {
      if (!stop_requested.load()) {
        core.DriveWrite();
      }
    }

    void DriveRead() noexcept {
      if (!stop_requested.load()) {
        core.DriveRead();
      }
    }

    void AdvanceHeartbeat(std::uint64_t now_ns) noexcept {
      if (!stop_requested.load()) {
        core.AdvanceHeartbeat(now_ns);
      }
    }

    bool ShouldReconnect() const noexcept {
      return stop_requested.load() || core.ShouldReconnect();
    }
  };

  void NotifyState(ConnectionPhase phase) noexcept {
    if (state_handler_ != nullptr) {
      state_handler_(state_context_, phase);
    }
  }

  void NotifyError(ConnectionError error) noexcept {
    if (error_handler_ != nullptr) {
      error_handler_(error_context_, error);
    }
  }

  ConnectionConfig config_{};
  MessageConsumer consumer_{};
  Metrics metrics_{};
  TlsSocket tls_socket_{};
  PreparedWriteArena prepared_write_arena_;
  CriticalSession<TlsSocket> core_;
  StateMachine state_machine_{};
  ColdPathLoop cold_path_loop_{};
  ActiveSpinLoop spin_loop_;
  std::array<char, 4096> handshake_storage_{};
  std::atomic<bool> stop_requested_{false};
  bool runtime_prepared_{false};
  ConnectionError prepare_error_{ConnectionError::kNone};
  void* state_context_{nullptr};
  StateHandler state_handler_{nullptr};
  void* error_context_{nullptr};
  ErrorHandler error_handler_{nullptr};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_GATE_WS_CLIENT_H_
