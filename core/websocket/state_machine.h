#ifndef AQUILA_CORE_WEBSOCKET_STATE_MACHINE_H_
#define AQUILA_CORE_WEBSOCKET_STATE_MACHINE_H_

#include "core/websocket/types.h"

namespace aquila::websocket {

// Thin connection state container only; it records phase/error but does not
// validate transitions or drive reconnect/close policy.
class StateMachine {
 public:
  ConnectionPhase phase() const noexcept { return phase_; }

  ConnectionError last_error() const noexcept { return last_error_; }

  void Enter(ConnectionPhase phase) noexcept {
    phase_ = phase;
    // A successful transition into active state explicitly clears the last
    // sticky error; other phase transitions preserve prior failure context.
    if (phase_ == ConnectionPhase::kActive) {
      last_error_ = ConnectionError::kNone;
    }
  }

  void Fail(ConnectionError error, ConnectionPhase phase) noexcept {
    last_error_ = error;
    phase_ = phase;
  }

 private:
  ConnectionPhase phase_{ConnectionPhase::kDisconnected};
  ConnectionError last_error_{ConnectionError::kNone};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_STATE_MACHINE_H_
