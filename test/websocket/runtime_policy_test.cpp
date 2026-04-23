#include "core/websocket/runtime_policy.h"

using namespace aquila::websocket;

namespace aquila::websocket {

bool ApplyRuntimePolicy(const RuntimePolicy& policy) noexcept;

}  // namespace aquila::websocket

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

  return 0;
}
