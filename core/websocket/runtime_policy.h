#ifndef AQUILA_CORE_WEBSOCKET_RUNTIME_POLICY_H_
#define AQUILA_CORE_WEBSOCKET_RUNTIME_POLICY_H_

namespace aquila::websocket {

enum class AffinityMode {
  kNone,
  kBestEffort,
  kRequired,
};

enum class SchedulingPolicy {
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
  int spin_iterations_before_clock_check = 4096;
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_RUNTIME_POLICY_H_
