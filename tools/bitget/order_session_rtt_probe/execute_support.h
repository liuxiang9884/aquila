#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_EXECUTE_SUPPORT_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_EXECUTE_SUPPORT_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/trading/order_feedback_event.h"
#include "tools/bitget/order_session_rtt_probe/sample_id_allocator.h"

namespace aquila::tools::bitget_order_session_rtt_probe {

struct ExecuteGuardInput {
  bool execute{false};
  bool live_preflight{false};
  bool confirm_dedicated_account{false};
  double duration_sec{0.0};
};

struct ExecuteGuardResult {
  bool ok{false};
  std::string error;
};

[[nodiscard]] inline ExecuteGuardResult ValidateExecuteGuard(
    const ExecuteGuardInput& input) {
  if (!std::isfinite(input.duration_sec) || input.duration_sec < 0.0) {
    return {.error = "--duration-sec must be finite and non-negative"};
  }
  if (input.execute && input.live_preflight) {
    return {.error = "--execute and --live-preflight are mutually exclusive"};
  }
  if (input.execute && !input.confirm_dedicated_account) {
    return {.error = "--execute requires --confirm-dedicated-account"};
  }
  if (input.execute && input.duration_sec <= 0.0) {
    return {.error = "--execute requires a positive --duration-sec"};
  }
  if (input.execute && input.duration_sec > 86400.0) {
    return {.error = "--execute limits --duration-sec to 86400 seconds"};
  }
  return {.ok = true};
}

struct RouteFeedbackResult {
  std::size_t consumed{0};
  std::size_t routed{0};
  std::size_t broadcast_deliveries{0};
  std::size_t unrouted{0};
  std::size_t queue_overflows{0};
  bool continuity_broadcast{false};
  bool failed{false};
};

template <typename ReaderT, typename QueueContainerT>
[[nodiscard]] RouteFeedbackResult RouteFeedback(
    ReaderT& reader, QueueContainerT& queues,
    std::size_t poll_budget) noexcept {
  RouteFeedbackResult result;
  result.consumed = reader.Poll(
      poll_budget,
      [&result, &queues](const OrderFeedbackEvent& event) noexcept {
        if (event.kind == OrderFeedbackKind::kContinuityLost) {
          result.continuity_broadcast = true;
          for (auto* queue : queues) {
            if (queue != nullptr && queue->TryPush(event)) {
              ++result.broadcast_deliveries;
            } else {
              ++result.queue_overflows;
              result.failed = true;
            }
          }
          return;
        }

        const auto session_index =
            SessionIndexForLocalOrderId(event.local_order_id, queues.size());
        if (!session_index.has_value() || *session_index >= queues.size() ||
            queues[*session_index] == nullptr) {
          ++result.unrouted;
          result.failed = true;
          return;
        }
        if (!queues[*session_index]->TryPush(event)) {
          ++result.queue_overflows;
          result.failed = true;
          return;
        }
        ++result.routed;
      });
  return result;
}

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_EXECUTE_SUPPORT_H_
