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

TEST(GateStrategyOrderFeedbackActionTest,
     DoesNotCancelAcceptedFeedbackAfterOrderFinished) {
  EXPECT_FALSE(ShouldSubmitCancelAfterFeedback(FeedbackCancelInput{
      .kind = OrderFeedbackKind::kAccepted,
      .order_known_after = true,
      .order_finished_after = true,
      .keep_open = false,
      .cancel_submitted = false,
  }));
}

TEST(GateStrategyOrderFeedbackActionTest, FinishesFromAppliedOrderState) {
  EXPECT_FALSE(ShouldFinishAfterFeedback(FeedbackFinishInput{
      .order_known_after = true,
      .order_finished_after = false,
      .wait_feedback_terminal = true,
  }));
  EXPECT_TRUE(ShouldFinishAfterFeedback(FeedbackFinishInput{
      .order_known_after = true,
      .order_finished_after = true,
      .wait_feedback_terminal = true,
  }));
  EXPECT_FALSE(ShouldFinishAfterFeedback(FeedbackFinishInput{
      .order_known_after = false,
      .order_finished_after = true,
      .wait_feedback_terminal = true,
  }));
}

}  // namespace
}  // namespace aquila::tools::gate_strategy_order
