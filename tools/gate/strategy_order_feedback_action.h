#ifndef AQUILA_TOOLS_GATE_STRATEGY_ORDER_FEEDBACK_ACTION_H_
#define AQUILA_TOOLS_GATE_STRATEGY_ORDER_FEEDBACK_ACTION_H_

#include "core/trading/order_feedback_event.h"

namespace aquila::tools::gate_strategy_order {

struct FeedbackCancelInput {
  OrderFeedbackKind kind{OrderFeedbackKind::kAccepted};
  bool order_known_after{false};
  bool order_finished_after{false};
  bool keep_open{false};
  bool cancel_submitted{false};
};

struct FeedbackFinishInput {
  bool order_known_after{false};
  bool order_finished_after{false};
  bool wait_feedback_terminal{false};
};

[[nodiscard]] inline bool ShouldSubmitCancelAfterFeedback(
    const FeedbackCancelInput& input) noexcept {
  return input.kind == OrderFeedbackKind::kAccepted &&
         input.order_known_after && !input.order_finished_after &&
         !input.keep_open && !input.cancel_submitted;
}

[[nodiscard]] inline bool ShouldFinishAfterFeedback(
    const FeedbackFinishInput& input) noexcept {
  return input.wait_feedback_terminal && input.order_known_after &&
         input.order_finished_after;
}

}  // namespace aquila::tools::gate_strategy_order

#endif  // AQUILA_TOOLS_GATE_STRATEGY_ORDER_FEEDBACK_ACTION_H_
