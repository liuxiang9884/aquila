#ifndef AQUILA_CORE_WEBSOCKET_RUNTIME_POLICY_H_
#define AQUILA_CORE_WEBSOCKET_RUNTIME_POLICY_H_

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(__linux__)
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

struct RuntimePolicy {
  AffinityMode affinity_mode = AffinityMode::kRequired;
  int io_cpu_id = -1;
  SchedulingPolicy scheduling_policy = SchedulingPolicy::kOther;
  int scheduling_priority = 0;
  bool lock_memory = true;
  bool prefault_stack = true;
  bool active_spin = true;
  std::uint32_t spin_iterations_before_clock_check = 4096;
};

inline void PrefaultThreadStack() noexcept {
#if defined(__linux__)
  constexpr size_t kPrefaultBytes = 64 * 1024;
  constexpr size_t kPageBytes = 4096;
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
  if (policy.affinity_mode != AffinityMode::kNone && policy.io_cpu_id >= 0) {
    if (policy.io_cpu_id >= CPU_SETSIZE) {
      return policy.affinity_mode != AffinityMode::kRequired;
    }

    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(policy.io_cpu_id, &cpu_set);
    if (::sched_setaffinity(0, sizeof(cpu_set), &cpu_set) != 0 &&
        policy.affinity_mode == AffinityMode::kRequired) {
      return false;
    }
  }

  if (policy.lock_memory && ::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    return false;
  }
#endif

  if (policy.prefault_stack) {
    PrefaultThreadStack();
  }

  return true;
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_RUNTIME_POLICY_H_
