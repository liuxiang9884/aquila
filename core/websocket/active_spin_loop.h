#ifndef AQUILA_CORE_WEBSOCKET_ACTIVE_SPIN_LOOP_H_
#define AQUILA_CORE_WEBSOCKET_ACTIVE_SPIN_LOOP_H_

#include <chrono>
#include <cstdint>
#include <thread>

#include "core/websocket/runtime_policy.h"

namespace aquila::websocket {

inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

class ActiveSpinLoop {
 public:
  explicit ActiveSpinLoop(const RuntimePolicy& runtime_policy) noexcept
      : runtime_policy_(runtime_policy) {}

  template <typename SessionT>
  void Run(SessionT& session) noexcept {
    std::uint32_t iteration_budget =
        runtime_policy_.spin_iterations_before_clock_check == 0
            ? 1
            : runtime_policy_.spin_iterations_before_clock_check;
    std::uint32_t iterations_since_clock = 0;

    while (!session.ShouldReconnect()) {
      session.DriveWrite();
      session.DriveRead();

      ++iterations_since_clock;
      if (iterations_since_clock >= iteration_budget) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        session.AdvanceHeartbeat(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
        iterations_since_clock = 0;
      }

      if (runtime_policy_.active_spin) {
        CpuRelax();
      } else {
        std::this_thread::yield();
      }
    }
  }

 private:
  RuntimePolicy runtime_policy_{};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_ACTIVE_SPIN_LOOP_H_
