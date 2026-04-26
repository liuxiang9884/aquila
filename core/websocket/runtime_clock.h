#ifndef AQUILA_CORE_WEBSOCKET_RUNTIME_CLOCK_H_
#define AQUILA_CORE_WEBSOCKET_RUNTIME_CLOCK_H_

#include <chrono>
#include <cstdint>

#if defined(__linux__)
#include <time.h>
#endif

#include "core/websocket/runtime_policy.h"

namespace aquila::websocket {

inline std::uint64_t SteadyClockNowNs() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

inline std::uint64_t NowNs(ClockSource source) noexcept {
  if (source == ClockSource::kSteady) {
    return SteadyClockNowNs();
  }
#if defined(__linux__)
  const clockid_t clock_id = source == ClockSource::kMonotonicCoarse
                                 ? CLOCK_MONOTONIC_COARSE
                                 : CLOCK_MONOTONIC;
  timespec ts{};
  if (::clock_gettime(clock_id, &ts) == 0) {
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
  }
#else
  (void)source;
#endif
  return SteadyClockNowNs();
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_RUNTIME_CLOCK_H_
