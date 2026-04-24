#include "core/websocket/runtime_policy.h"

#include <gtest/gtest.h>

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

TEST(WebsocketRuntimePolicyTest, AppliesExpectedRuntimeConstraints) {
  RuntimePolicy policy{};
  policy.affinity_mode = AffinityMode::kNone;
  policy.lock_memory = false;
  policy.prefault_stack = false;
  EXPECT_TRUE(ApplyRuntimePolicy(policy));

  const RuntimePolicy default_policy{};
  EXPECT_FALSE(ApplyRuntimePolicy(default_policy));

#if defined(__linux__)
  RuntimePolicy required_invalid_cpu = policy;
  required_invalid_cpu.affinity_mode = AffinityMode::kRequired;
  required_invalid_cpu.io_cpu_id = CPU_SETSIZE;
  EXPECT_FALSE(ApplyRuntimePolicy(required_invalid_cpu));

  RuntimePolicy best_effort_invalid_cpu = policy;
  best_effort_invalid_cpu.affinity_mode = AffinityMode::kBestEffort;
  best_effort_invalid_cpu.io_cpu_id = CPU_SETSIZE;
  EXPECT_TRUE(ApplyRuntimePolicy(best_effort_invalid_cpu));

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
  EXPECT_FALSE(ApplyRuntimePolicy(rollback_policy));
  EXPECT_EQ(g_mlockall_calls, 1);
  EXPECT_EQ(g_pthread_setaffinity_calls, 1);
  EXPECT_EQ(g_munlockall_calls, 1);
#endif
}
