#include "core/websocket/runtime_policy.h"

#if defined(__linux__)
#include <sched.h>
#endif

using namespace aquila::websocket;

int main() {
  RuntimePolicy policy{};
  policy.affinity_mode = AffinityMode::kNone;
  policy.lock_memory = false;
  policy.prefault_stack = false;
  if (!ApplyRuntimePolicy(policy)) {
    return 1;
  }

  const RuntimePolicy default_policy{};
  if (ApplyRuntimePolicy(default_policy)) {
    return 1;
  }

#if defined(__linux__)
  RuntimePolicy required_invalid_cpu = policy;
  required_invalid_cpu.affinity_mode = AffinityMode::kRequired;
  required_invalid_cpu.io_cpu_id = CPU_SETSIZE;
  if (ApplyRuntimePolicy(required_invalid_cpu)) {
    return 1;
  }

  RuntimePolicy best_effort_invalid_cpu = policy;
  best_effort_invalid_cpu.affinity_mode = AffinityMode::kBestEffort;
  best_effort_invalid_cpu.io_cpu_id = CPU_SETSIZE;
  if (!ApplyRuntimePolicy(best_effort_invalid_cpu)) {
    return 1;
  }
#endif

  return 0;
}
