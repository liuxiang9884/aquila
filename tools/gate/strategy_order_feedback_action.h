#ifndef AQUILA_TOOLS_GATE_STRATEGY_ORDER_FEEDBACK_ACTION_H_
#define AQUILA_TOOLS_GATE_STRATEGY_ORDER_FEEDBACK_ACTION_H_

#include "core/trading/order_feedback_event.h"

namespace aquila::tools::gate_strategy_order {

struct FeedbackCancelInput {
  OrderFeedbackKind kind{OrderFeedbackKind::kAccepted};
  bool order_known_after{false};
  bool keep_open{false};
  bool cancel_submitted{false};
};

[[nodiscard]] inline bool IsTerminalOrderFeedback(
    OrderFeedbackKind kind) noexcept {
  return kind == OrderFeedbackKind::kFilled ||
         kind == OrderFeedbackKind::kCancelled ||
         kind == OrderFeedbackKind::kRejected;
}

[[nodiscard]] inline bool ShouldSubmitCancelAfterFeedback(
    const FeedbackCancelInput& input) noexcept {
  return input.kind == OrderFeedbackKind::kAccepted &&
         input.order_known_after && !input.keep_open && !input.cancel_submitted;
}

}  // namespace aquila::tools::gate_strategy_order

#endif  // AQUILA_TOOLS_GATE_STRATEGY_ORDER_FEEDBACK_ACTION_H_
