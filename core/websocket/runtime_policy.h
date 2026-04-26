#ifndef AQUILA_CORE_WEBSOCKET_RUNTIME_POLICY_H_
#define AQUILA_CORE_WEBSOCKET_RUNTIME_POLICY_H_

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif

namespace aquila::websocket {

enum class AffinityMode : std::uint8_t {
  kNone,
  kBestEffort,
  kRequired,
};

enum class SchedulingPolicy : std::uint8_t {
  kOther,
  kFifo,
  kRoundRobin,
};

enum class ClockSource : std::uint8_t {
  kSteady,
  kMonotonic,
  kMonotonicCoarse,
};

struct RuntimePolicy {
  AffinityMode affinity_mode = AffinityMode::kRequired;
  int io_cpu_id = -1;
  SchedulingPolicy scheduling_policy = SchedulingPolicy::kOther;
  int scheduling_priority = 0;
  bool lock_memory = true;
  bool prefault_stack = true;
  bool active_spin = true;
  std::uint32_t spin_iterations_before_clock_check = 4096;
  ClockSource clock_source = ClockSource::kSteady;
};

#if defined(__linux__)
namespace detail {

struct RuntimePolicySyscallHooks {
  int (*mlockall)(int) = &::mlockall;
  int (*munlockall)() = &::munlockall;
  int (*pthread_setaffinity_np)(pthread_t, size_t, const cpu_set_t*) =
      &::pthread_setaffinity_np;
};

inline RuntimePolicySyscallHooks& RuntimePolicySyscallHooksForTest() noexcept {
  static RuntimePolicySyscallHooks hooks;
  return hooks;
}

inline void ResetRuntimePolicySyscallHooksForTest() noexcept {
  RuntimePolicySyscallHooksForTest() = RuntimePolicySyscallHooks{};
}

}  // namespace detail
#endif

inline void PrefaultThreadStack() noexcept {
#if defined(__linux__)
  constexpr size_t kPrefaultBytes = 64 * 1024;
  constexpr size_t kPageBytes = 4096;
  // Best-effort only: touch a fixed stack window, not the full thread stack.
  alignas(64) std::array<std::byte, kPrefaultBytes> buffer{};
  volatile std::byte* prefault_bytes = buffer.data();
  for (size_t offset = 0; offset < buffer.size(); offset += kPageBytes) {
    prefault_bytes[offset] = std::byte{0};
  }
#endif
}

inline bool ApplyRuntimePolicy(const RuntimePolicy& policy) noexcept {
  if (policy.affinity_mode == AffinityMode::kRequired && policy.io_cpu_id < 0) {
    return false;
  }

#if defined(__linux__)
  if (policy.io_cpu_id >= CPU_SETSIZE &&
      policy.affinity_mode == AffinityMode::kRequired) {
    return false;
  }

  bool memory_locked = false;
  detail::RuntimePolicySyscallHooks& hooks =
      detail::RuntimePolicySyscallHooksForTest();
  if (policy.lock_memory) {
    if (hooks.mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
      return false;
    }
    memory_locked = true;
  }

  if (policy.affinity_mode != AffinityMode::kNone && policy.io_cpu_id >= 0 &&
      policy.io_cpu_id < CPU_SETSIZE) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(policy.io_cpu_id, &cpu_set);
    if (hooks.pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set),
                                     &cpu_set) != 0 &&
        policy.affinity_mode == AffinityMode::kRequired) {
      if (memory_locked) {
        hooks.munlockall();
      }
      return false;
    }
  }

  if (policy.prefault_stack) {
    PrefaultThreadStack();
  }
#else
  if (policy.prefault_stack) {
    PrefaultThreadStack();
  }
#endif

  return true;
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_RUNTIME_POLICY_H_
