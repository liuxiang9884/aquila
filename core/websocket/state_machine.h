#ifndef AQUILA_CORE_WEBSOCKET_STATE_MACHINE_H_
#define AQUILA_CORE_WEBSOCKET_STATE_MACHINE_H_

#include "core/websocket/types.h"

namespace aquila::websocket {

class StateMachine {
 public:
  ConnectionPhase phase() const noexcept { return phase_; }

  ConnectionError last_error() const noexcept { return last_error_; }

  void Enter(ConnectionPhase phase) noexcept {
    phase_ = phase;
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
