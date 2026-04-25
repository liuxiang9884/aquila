#include "core/websocket/degraded_evaluator.h"

#include <gtest/gtest.h>

#include <cstdint>

using namespace aquila::websocket;

namespace {

constexpr std::uint64_t kMs = 1'000'000ULL;
constexpr std::uint64_t kSecond = 1'000'000'000ULL;

DegradedSample BuildSample(std::uint64_t now_ns) noexcept {
  DegradedSample sample{};
  sample.now_ns = now_ns;
  sample.prepared_write_slots = 10;
  return sample;
}

DegradedThresholds BuildThresholds() noexcept {
  DegradedThresholds thresholds{};
  thresholds.high_watermark_percent = 80;
  thresholds.high_watermark_hold_ticks = 3;
  thresholds.recover_ticks = 3;
  thresholds.backpressure_drops_per_second = 10;
  thresholds.awaiting_pong_timeout_ms = 100;
  return thresholds;
}

}  // namespace

TEST(WebsocketDegradedEvaluatorTest,
     EntersAfterHighWatermarkHoldsForConfiguredTicks) {
  auto thresholds = BuildThresholds();
  thresholds.backpressure_drops_per_second = 0;
  thresholds.awaiting_pong_timeout_ms = 0;
  DegradedEvaluator evaluator(thresholds);

  auto sample = BuildSample(kSecond);
  sample.pending_write_count = 8;

  auto result = evaluator.Evaluate(sample);
  EXPECT_FALSE(result.active);
  EXPECT_FALSE(result.entered);

  sample.now_ns += kMs;
  result = evaluator.Evaluate(sample);
  EXPECT_FALSE(result.active);
  EXPECT_FALSE(result.entered);

  sample.now_ns += kMs;
  result = evaluator.Evaluate(sample);
  EXPECT_TRUE(result.active);
  EXPECT_TRUE(result.entered);
  EXPECT_FALSE(result.exited);
}

TEST(WebsocketDegradedEvaluatorTest, EntersOnBackpressureDropWindow) {
  auto thresholds = BuildThresholds();
  thresholds.high_watermark_percent = 0;
  thresholds.awaiting_pong_timeout_ms = 0;
  DegradedEvaluator evaluator(thresholds);

  auto sample = BuildSample(kSecond);
  sample.consumer_backpressure_drops = 0;
  EXPECT_FALSE(evaluator.Evaluate(sample).active);

  sample.now_ns += 500 * kMs;
  sample.consumer_backpressure_drops = 10;
  const auto result = evaluator.Evaluate(sample);
  EXPECT_TRUE(result.active);
  EXPECT_TRUE(result.entered);
}

TEST(WebsocketDegradedEvaluatorTest, EntersOnAwaitingPongTimeout) {
  auto thresholds = BuildThresholds();
  thresholds.high_watermark_percent = 0;
  thresholds.backpressure_drops_per_second = 0;
  DegradedEvaluator evaluator(thresholds);

  auto sample = BuildSample(kSecond + 101 * kMs);
  sample.awaiting_pong = true;
  sample.last_ping_ns = kSecond;

  const auto result = evaluator.Evaluate(sample);
  EXPECT_TRUE(result.active);
  EXPECT_TRUE(result.entered);
}

TEST(WebsocketDegradedEvaluatorTest, MultipleTriggersEnterOnlyOnce) {
  auto thresholds = BuildThresholds();
  thresholds.high_watermark_hold_ticks = 1;
  DegradedEvaluator evaluator(thresholds);

  auto sample = BuildSample(kSecond);
  sample.pending_write_count = 0;
  sample.awaiting_pong = false;
  sample.last_ping_ns = 0;
  sample.consumer_backpressure_drops = 0;
  EXPECT_FALSE(evaluator.Evaluate(sample).active);

  sample.now_ns += kMs;
  sample.pending_write_count = 10;
  sample.awaiting_pong = true;
  sample.last_ping_ns = kSecond - 200 * kMs;
  sample.consumer_backpressure_drops = 12;
  auto result = evaluator.Evaluate(sample);
  EXPECT_TRUE(result.active);
  EXPECT_TRUE(result.entered);

  sample.now_ns += kMs;
  sample.consumer_backpressure_drops = 20;
  result = evaluator.Evaluate(sample);
  EXPECT_TRUE(result.active);
  EXPECT_FALSE(result.entered);
  EXPECT_FALSE(result.exited);
}

TEST(WebsocketDegradedEvaluatorTest,
     ExitsAfterAllTriggersRecoverForConfiguredTicks) {
  auto thresholds = BuildThresholds();
  thresholds.high_watermark_percent = 0;
  thresholds.backpressure_drops_per_second = 0;
  thresholds.recover_ticks = 3;
  DegradedEvaluator evaluator(thresholds);

  auto sample = BuildSample(kSecond + 101 * kMs);
  sample.awaiting_pong = true;
  sample.last_ping_ns = kSecond;
  ASSERT_TRUE(evaluator.Evaluate(sample).entered);

  sample.awaiting_pong = false;
  for (int i = 0; i < 2; ++i) {
    sample.now_ns += kMs;
    const auto result = evaluator.Evaluate(sample);
    EXPECT_TRUE(result.active);
    EXPECT_FALSE(result.exited);
  }

  sample.now_ns += kMs;
  const auto result = evaluator.Evaluate(sample);
  EXPECT_FALSE(result.active);
  EXPECT_FALSE(result.entered);
  EXPECT_TRUE(result.exited);
}

TEST(WebsocketDegradedEvaluatorTest, OscillationDoesNotToggleBeforeRecovery) {
  auto thresholds = BuildThresholds();
  thresholds.high_watermark_percent = 80;
  thresholds.high_watermark_hold_ticks = 1;
  thresholds.backpressure_drops_per_second = 0;
  thresholds.awaiting_pong_timeout_ms = 0;
  thresholds.recover_ticks = 3;
  DegradedEvaluator evaluator(thresholds);

  auto sample = BuildSample(kSecond);
  sample.pending_write_count = 8;
  EXPECT_TRUE(evaluator.Evaluate(sample).entered);

  sample.now_ns += kMs;
  sample.pending_write_count = 0;
  EXPECT_TRUE(evaluator.Evaluate(sample).active);

  sample.now_ns += kMs;
  sample.pending_write_count = 8;
  auto result = evaluator.Evaluate(sample);
  EXPECT_TRUE(result.active);
  EXPECT_FALSE(result.entered);

  sample.pending_write_count = 0;
  for (int i = 0; i < 2; ++i) {
    sample.now_ns += kMs;
    result = evaluator.Evaluate(sample);
    EXPECT_TRUE(result.active);
    EXPECT_FALSE(result.exited);
  }
  sample.now_ns += kMs;
  result = evaluator.Evaluate(sample);
  EXPECT_FALSE(result.active);
  EXPECT_TRUE(result.exited);
}

TEST(WebsocketDegradedEvaluatorTest, BackpressureWindowExpiresOldDrops) {
  auto thresholds = BuildThresholds();
  thresholds.high_watermark_percent = 0;
  thresholds.awaiting_pong_timeout_ms = 0;
  thresholds.recover_ticks = 1;
  DegradedEvaluator evaluator(thresholds);

  auto sample = BuildSample(kSecond);
  sample.consumer_backpressure_drops = 0;
  EXPECT_FALSE(evaluator.Evaluate(sample).active);

  sample.now_ns += 500 * kMs;
  sample.consumer_backpressure_drops = 10;
  EXPECT_TRUE(evaluator.Evaluate(sample).entered);

  sample.now_ns += 2 * kSecond;
  const auto result = evaluator.Evaluate(sample);
  EXPECT_FALSE(result.active);
  EXPECT_TRUE(result.exited);
}

TEST(WebsocketDegradedEvaluatorTest, ZeroThresholdsDisableTriggers) {
  DegradedThresholds thresholds{};
  thresholds.high_watermark_percent = 0;
  thresholds.high_watermark_hold_ticks = 1;
  thresholds.recover_ticks = 1;
  thresholds.backpressure_drops_per_second = 0;
  thresholds.awaiting_pong_timeout_ms = 0;
  DegradedEvaluator evaluator(thresholds);

  auto sample = BuildSample(kSecond);
  sample.pending_write_count = 10;
  sample.awaiting_pong = true;
  sample.last_ping_ns = 1;

  for (int i = 0; i < 20; ++i) {
    sample.now_ns += 100 * kMs;
    sample.consumer_backpressure_drops += 100;
    const auto result = evaluator.Evaluate(sample);
    EXPECT_FALSE(result.active);
    EXPECT_FALSE(result.entered);
    EXPECT_FALSE(result.exited);
  }
}
