#ifndef AQUILA_CORE_WEBSOCKET_ACTIVE_SPIN_LOOP_H_
#define AQUILA_CORE_WEBSOCKET_ACTIVE_SPIN_LOOP_H_

#include <cstdint>
#include <thread>

#include "core/websocket/runtime_clock.h"

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
    iteration_budget = ClockCheckInterval(session, iteration_budget);
    std::uint32_t iterations_since_clock = 0;

    while (!session.ShouldReconnect()) {
      session.DriveWrite();
      session.DriveRead();

      ++iterations_since_clock;
      if (iterations_since_clock >= iteration_budget) {
        AdvanceClock(session, NowNs(runtime_policy_.clock_source),
                     iterations_since_clock);
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
  template <typename SessionT>
  std::uint32_t ClockCheckInterval(const SessionT& session,
                                   std::uint32_t default_interval) const
      noexcept {
    std::uint32_t interval = default_interval;
    if constexpr (requires {
                    session.ClockCheckInterval(default_interval);
                  }) {
      interval = session.ClockCheckInterval(default_interval);
    }
    return interval == 0 ? 1 : interval;
  }

  template <typename SessionT>
  void AdvanceClock(SessionT& session, std::uint64_t now_ns,
                    std::uint32_t elapsed_iterations) const noexcept {
    if constexpr (requires {
                    session.AdvanceClock(now_ns, elapsed_iterations);
                  }) {
      session.AdvanceClock(now_ns, elapsed_iterations);
    } else {
      (void)elapsed_iterations;
      session.AdvanceHeartbeat(now_ns);
    }
  }

  RuntimePolicy runtime_policy_{};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_ACTIVE_SPIN_LOOP_H_
