#include <cstdint>

#include <gtest/gtest.h>

#include "strategy/lead_lag/alignment.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/recorders.h"
#include "strategy/lead_lag/threshold.h"
#include "strategy/lead_lag/types.h"

namespace {

namespace leadlag = aquila::strategy::leadlag;

leadlag::QuoteSnapshot Quote(std::int64_t event_ns, double bid_price,
                             double ask_price) {
  return leadlag::QuoteSnapshot{
      .event_ns = event_ns,
      .bid_price = bid_price,
      .ask_price = ask_price,
  };
}

leadlag::PairConfig PairConfigForThreshold() {
  leadlag::PairConfig pair;
  pair.lag_taker_fee = 0.0001;
  pair.trigger = leadlag::TriggerConfig{
      .lead = 0.0025,
      .close = 0.0005,
      .quantile =
          leadlag::QuantileConfig{
              .move = 0.6,
              .up_min = 0.0,
              .up_max = 0.10,
              .down_min = -0.10,
              .down_max = 0.0,
              .up_bins = 10,
              .down_bins = 10,
          },
  };
  pair.bbo_record = leadlag::BboRecordConfig{
      .window_ns = 10,
      .stats_window_ns = 100,
  };
  return pair;
}

TEST(LeadLagThresholdTest, SeedsInitialThresholdsFromTrigger) {
  leadlag::ThresholdState threshold;
  threshold.Init(PairConfigForThreshold());

  const leadlag::ThresholdSnapshot snapshot = threshold.snapshot();
  EXPECT_TRUE(snapshot.initialized);
  EXPECT_DOUBLE_EQ(snapshot.up_entry, 0.0025);
  EXPECT_DOUBLE_EQ(snapshot.up_exit, 0.0005);
  EXPECT_DOUBLE_EQ(snapshot.down_entry, -0.0025);
  EXPECT_DOUBLE_EQ(snapshot.down_exit, -0.0005);
  EXPECT_EQ(snapshot.roll_count, 0U);
}

TEST(LeadLagThresholdTest, RollsOldMoveWindowAndKeepsCurrentTickInNewWindow) {
  const leadlag::PairConfig pair = PairConfigForThreshold();

  leadlag::MoveQuantileWindow moves;
  moves.Init(/*start_ns=*/0, pair.bbo_record.stats_window_ns,
             pair.trigger.quantile);
  const leadlag::BboExtremaSnapshot extrema{
      .valid = true,
      .bid_min = 100.0,
      .ask_max = 110.0,
  };
  EXPECT_FALSE(moves
                   .Update(Quote(/*event_ns=*/10, /*bid_price=*/101.1,
                                 /*ask_price=*/107.69),
                           extrema)
                   .rolled);
  EXPECT_FALSE(moves
                   .Update(Quote(/*event_ns=*/100, /*bid_price=*/102.9,
                                 /*ask_price=*/105.71),
                           extrema)
                   .rolled);

  leadlag::ThresholdState threshold;
  threshold.Init(pair);
  const leadlag::RecorderSnapshot recorder{
      .lead_noise = 0.001,
      .lag_noise = 0.002,
  };
  const leadlag::AlignmentSnapshot alignment{
      .drift_std_ema = 0.005,
  };

  const leadlag::MoveQuantileRoll roll =
      moves.Update(Quote(/*event_ns=*/101, /*bid_price=*/101.5,
                         /*ask_price=*/106.7),
                   extrema);
  const leadlag::ThresholdSnapshot snapshot =
      threshold.OnMoveRoll(roll, recorder, alignment);

  ASSERT_TRUE(roll.rolled);
  EXPECT_EQ(moves.sample_count(), 1U);
  EXPECT_EQ(snapshot.roll_count, 1U);
  EXPECT_DOUBLE_EQ(snapshot.last_up_quantile, 0.03);
  EXPECT_NEAR(snapshot.last_down_quantile, -0.04, 1e-15);
  EXPECT_DOUBLE_EQ(snapshot.lead_noise, 0.001);
  EXPECT_DOUBLE_EQ(snapshot.lag_noise, 0.002);
  EXPECT_DOUBLE_EQ(snapshot.drift_std_ema, 0.005);
  EXPECT_DOUBLE_EQ(snapshot.last_profit_buffer, 0.0032);
  EXPECT_DOUBLE_EQ(snapshot.up_entry, 0.03);
  EXPECT_DOUBLE_EQ(snapshot.up_exit, 0.005);
  EXPECT_NEAR(snapshot.down_entry, -0.04, 1e-15);
  EXPECT_DOUBLE_EQ(snapshot.down_exit, -0.005);
}

TEST(LeadLagThresholdTest, ProfitBufferSetsFallbackThresholds) {
  const leadlag::PairConfig pair = PairConfigForThreshold();
  leadlag::ThresholdState threshold;
  threshold.Init(pair);

  const leadlag::RecorderSnapshot recorder{
      .lead_noise = 0.001,
      .lag_noise = 0.002,
  };
  const leadlag::AlignmentSnapshot alignment{
      .drift_std_ema = 0.005,
  };

  const leadlag::ThresholdSnapshot snapshot = threshold.OnMoveRoll(
      leadlag::MoveQuantileRoll{
          .rolled = true,
          .up_quantile = 0.0,
          .down_quantile = 0.0,
          .roll_at_ns = 200,
      },
      recorder, alignment);

  EXPECT_DOUBLE_EQ(snapshot.last_profit_buffer, 0.0032);
  EXPECT_DOUBLE_EQ(snapshot.up_entry, 0.0082);
  EXPECT_DOUBLE_EQ(snapshot.up_exit, 0.005);
  EXPECT_DOUBLE_EQ(snapshot.down_entry, -0.0082);
  EXPECT_DOUBLE_EQ(snapshot.down_exit, -0.005);
}

}  // namespace
