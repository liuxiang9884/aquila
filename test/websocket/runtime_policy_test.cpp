#include "core/websocket/runtime_policy.h"

#if defined(__linux__)
#include <cstddef>
#include <sched.h>
#endif

using namespace aquila::websocket;

#if defined(__linux__)
namespace {

int g_mlockall_calls = 0;
int g_pthread_setaffinity_calls = 0;
int g_munlockall_calls = 0;

int FakeMlockall(int) noexcept {
  ++g_mlockall_calls;
  return 0;
}

int FakePthreadSetAffinity(pthread_t, size_t, const cpu_set_t*) noexcept {
  ++g_pthread_setaffinity_calls;
  return -1;
}

int FakeMunlockall() noexcept {
  ++g_munlockall_calls;
  return 0;
}

struct RuntimePolicyHookReset {
  ~RuntimePolicyHookReset() { detail::ResetRuntimePolicySyscallHooksForTest(); }
};

}  // namespace
#endif

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

  RuntimePolicyHookReset hook_reset;
  detail::RuntimePolicySyscallHooks& hooks =
      detail::RuntimePolicySyscallHooksForTest();
  hooks.mlockall = &FakeMlockall;
  hooks.pthread_setaffinity_np = &FakePthreadSetAffinity;
  hooks.munlockall = &FakeMunlockall;

  g_mlockall_calls = 0;
  g_pthread_setaffinity_calls = 0;
  g_munlockall_calls = 0;

  RuntimePolicy rollback_policy{};
  rollback_policy.affinity_mode = AffinityMode::kRequired;
  rollback_policy.io_cpu_id = 0;
  rollback_policy.lock_memory = true;
  rollback_policy.prefault_stack = false;
  if (ApplyRuntimePolicy(rollback_policy)) {
    return 1;
  }
  if (g_mlockall_calls != 1 || g_pthread_setaffinity_calls != 1 ||
      g_munlockall_calls != 1) {
    return 1;
  }
#endif

  return 0;
}
