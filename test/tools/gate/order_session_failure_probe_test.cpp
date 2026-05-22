#include "tools/gate/order_session_failure_probe.h"

#include <gtest/gtest.h>

namespace aquila::tools::gate_order_session_failure_probe {
namespace {

TEST(GateOrderSessionFailureProbeTest, ParsesProbeModes) {
  ProbeMode parsed{};
  EXPECT_TRUE(ParseProbeMode("submit-rejected", &parsed));
  EXPECT_EQ(parsed, ProbeMode::kSubmitRejected);
  EXPECT_TRUE(ParseProbeMode("cancel-rejected", &parsed));
  EXPECT_EQ(parsed, ProbeMode::kCancelRejected);
  EXPECT_FALSE(ParseProbeMode("rejected", &parsed));
}

TEST(GateOrderSessionFailureProbeTest, SubmitRejectedCompletesOnlyOnRejected) {
  EXPECT_FALSE(ResolveProbeResponseDecision(
                   ProbeResponseInput{.mode = ProbeMode::kSubmitRejected,
                                      .kind = gate::OrderResponseKind::kAck})
                   .finish);

  const ProbeResponseDecision rejected = ResolveProbeResponseDecision(
      ProbeResponseInput{.mode = ProbeMode::kSubmitRejected,
                         .kind = gate::OrderResponseKind::kRejected});

  EXPECT_TRUE(rejected.finish);
  EXPECT_EQ(rejected.exit_code, 0);
}

TEST(GateOrderSessionFailureProbeTest,
     SubmitRejectedUnexpectedAcceptedRequestsSafetyCancel) {
  const ProbeResponseDecision accepted = ResolveProbeResponseDecision(
      ProbeResponseInput{.mode = ProbeMode::kSubmitRejected,
                         .kind = gate::OrderResponseKind::kAccepted,
                         .keep_open = false,
                         .safety_cancel_submitted = false});

  EXPECT_FALSE(accepted.finish);
  EXPECT_TRUE(accepted.submit_safety_cancel);
}

TEST(GateOrderSessionFailureProbeTest,
     SubmitRejectedUnexpectedTerminalCancelFails) {
  const ProbeResponseDecision decision = ResolveProbeResponseDecision(
      ProbeResponseInput{.mode = ProbeMode::kSubmitRejected,
                         .kind = gate::OrderResponseKind::kCancelAccepted,
                         .safety_cancel_submitted = true});

  EXPECT_TRUE(decision.finish);
  EXPECT_EQ(decision.exit_code, 1);
}

TEST(GateOrderSessionFailureProbeTest,
     CancelRejectedCompletesOnlyOnCancelRejected) {
  EXPECT_FALSE(ResolveProbeResponseDecision(
                   ProbeResponseInput{.mode = ProbeMode::kCancelRejected,
                                      .kind = gate::OrderResponseKind::kAck})
                   .finish);

  const ProbeResponseDecision rejected = ResolveProbeResponseDecision(
      ProbeResponseInput{.mode = ProbeMode::kCancelRejected,
                         .kind = gate::OrderResponseKind::kCancelRejected});
  const ProbeResponseDecision accepted = ResolveProbeResponseDecision(
      ProbeResponseInput{.mode = ProbeMode::kCancelRejected,
                         .kind = gate::OrderResponseKind::kCancelAccepted});

  EXPECT_TRUE(rejected.finish);
  EXPECT_EQ(rejected.exit_code, 0);
  EXPECT_TRUE(accepted.finish);
  EXPECT_EQ(accepted.exit_code, 1);
}

}  // namespace
}  // namespace aquila::tools::gate_order_session_failure_probe
