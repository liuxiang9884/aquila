#include "tools/bitget/private_session_probe_outcome.h"

#include <gtest/gtest.h>

namespace aquila::bitget {
namespace {

TEST(BitgetPrivateSessionProbeOutcomeTest,
     RequiresRequestedDurationToComplete) {
  const PrivateSessionProbeOutcome early_exit{
      .started_ok = true,
      .completed_requested_duration = false,
      .reached_ready = true,
      .response_stream_clean = true,
  };
  PrivateSessionProbeOutcome controlled_stop = early_exit;
  controlled_stop.completed_requested_duration = true;

  EXPECT_FALSE(PrivateSessionProbeSucceeded(early_exit));
  EXPECT_TRUE(PrivateSessionProbeSucceeded(controlled_stop));
}

TEST(BitgetPrivateSessionProbeOutcomeTest,
     RejectsStartReadyAndResponseFailures) {
  PrivateSessionProbeOutcome outcome{
      .started_ok = true,
      .completed_requested_duration = true,
      .reached_ready = true,
      .response_stream_clean = true,
  };

  outcome.started_ok = false;
  EXPECT_FALSE(PrivateSessionProbeSucceeded(outcome));
  outcome.started_ok = true;
  outcome.reached_ready = false;
  EXPECT_FALSE(PrivateSessionProbeSucceeded(outcome));
  outcome.reached_ready = true;
  outcome.response_stream_clean = false;
  EXPECT_FALSE(PrivateSessionProbeSucceeded(outcome));
}

}  // namespace
}  // namespace aquila::bitget
