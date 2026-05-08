#include "tools/gate/strategy_order_feedback_action.h"

#include <gtest/gtest.h>

namespace aquila::tools::gate_strategy_order {
namespace {

TEST(GateStrategyOrderFeedbackActionTest,
     CancelsKnownAcceptedFeedbackWhenOrderShouldNotStayOpen) {
  EXPECT_TRUE(ShouldSubmitCancelAfterFeedback(FeedbackCancelInput{
      .kind = OrderFeedbackKind::kAccepted,
      .order_known_after = true,
      .keep_open = false,
      .cancel_submitted = false,
  }));
}

TEST(GateStrategyOrderFeedbackActionTest, DoesNotCancelWhenKeepOpen) {
  EXPECT_FALSE(ShouldSubmitCancelAfterFeedback(FeedbackCancelInput{
      .kind = OrderFeedbackKind::kAccepted,
      .order_known_after = true,
      .keep_open = true,
      .cancel_submitted = false,
  }));
}

TEST(GateStrategyOrderFeedbackActionTest, DoesNotCancelUnknownOrder) {
  EXPECT_FALSE(ShouldSubmitCancelAfterFeedback(FeedbackCancelInput{
      .kind = OrderFeedbackKind::kAccepted,
      .order_known_after = false,
      .keep_open = false,
      .cancel_submitted = false,
  }));
}

TEST(GateStrategyOrderFeedbackActionTest, DoesNotCancelTerminalFeedback) {
  EXPECT_FALSE(ShouldSubmitCancelAfterFeedback(FeedbackCancelInput{
      .kind = OrderFeedbackKind::kCancelled,
      .order_known_after = true,
      .keep_open = false,
      .cancel_submitted = false,
  }));
}

TEST(GateStrategyOrderFeedbackActionTest, DetectsTerminalFeedback) {
  EXPECT_FALSE(IsTerminalOrderFeedback(OrderFeedbackKind::kAccepted));
  EXPECT_TRUE(IsTerminalOrderFeedback(OrderFeedbackKind::kFilled));
  EXPECT_TRUE(IsTerminalOrderFeedback(OrderFeedbackKind::kCancelled));
  EXPECT_TRUE(IsTerminalOrderFeedback(OrderFeedbackKind::kRejected));
}

}  // namespace
}  // namespace aquila::tools::gate_strategy_order
