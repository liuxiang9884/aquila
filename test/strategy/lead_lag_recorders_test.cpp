#include <cmath>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/recorders.h"
#include "strategy/lead_lag/types.h"
#include "strategy/lead_lag/window_stats.h"

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

leadlag::PairConfig PairConfigForRecorder() {
  leadlag::PairConfig pair;
  pair.bbo_record = leadlag::BboRecordConfig{
      .window_ns = 10,
      .stats_window_ns = 100,
  };
  pair.capacity = leadlag::CapacityConfig{
      .extrema_window_capacity = 2,
      .move_queue_capacity = 2,
      .noise_window_capacity = 2,
      .spread_window_capacity = 2,
  };
  pair.trigger.quantile = leadlag::QuantileConfig{
      .move = 0.5,
      .up_min = 0.0,
      .up_max = 0.10,
      .down_min = -0.10,
      .down_max = 0.0,
      .precision = 0.01,
      .up_bins = 10,
      .down_bins = 10,
  };
  return pair;
}

TEST(LeadLagWindowStatsTest, MeanWindowEvictsByTimeAndKeepsBoundarySample) {
  leadlag::MeanWindow window;
  window.Init(/*window_ns=*/10, /*capacity=*/2);

  window.Update(/*event_ns=*/0, /*value=*/10.0);
  window.Update(/*event_ns=*/10, /*value=*/20.0);
  EXPECT_EQ(window.size(), 2U);
  EXPECT_DOUBLE_EQ(window.mean(), 15.0);

  window.Update(/*event_ns=*/11, /*value=*/30.0);
  EXPECT_EQ(window.size(), 2U);
  EXPECT_DOUBLE_EQ(window.mean(), 25.0);
}

TEST(LeadLagWindowStatsTest, MeanWindowsEvictExpiredBeforePushing) {
  leadlag::MeanWindow mean;
  mean.Init(/*window_ns=*/10, /*capacity=*/2);
  mean.Update(/*event_ns=*/0, /*value=*/10.0);
  mean.Update(/*event_ns=*/10, /*value=*/20.0);
  mean.Update(/*event_ns=*/11, /*value=*/30.0);

  EXPECT_EQ(mean.size(), 2U);
  EXPECT_EQ(mean.capacity(), 2U);

  leadlag::MeanStdWindow mean_std;
  mean_std.Init(/*window_ns=*/10, /*capacity=*/2);
  mean_std.Update(/*event_ns=*/0, /*value=*/10.0);
  mean_std.Update(/*event_ns=*/10, /*value=*/20.0);
  mean_std.Update(/*event_ns=*/11, /*value=*/30.0);

  EXPECT_EQ(mean_std.size(), 2U);
  EXPECT_EQ(mean_std.capacity(), 2U);
}

TEST(LeadLagWindowStatsTest, MeanStdWindowComputesPopulationStd) {
  leadlag::MeanStdWindow window;
  window.Init(/*window_ns=*/100, /*capacity=*/2);

  for (const double value : {1.0, 2.0, 3.0, 4.0}) {
    window.Update(/*event_ns=*/1, value);
  }

  EXPECT_EQ(window.size(), 4U);
  EXPECT_GE(window.capacity(), 4U);
  EXPECT_DOUBLE_EQ(window.mean(), 2.5);
  EXPECT_DOUBLE_EQ(window.stddev(), std::sqrt(1.25));
}

TEST(LeadLagWindowStatsTest, CapacityGrowPreservesMeanAndStdResults) {
  leadlag::MeanWindow grown_mean;
  leadlag::MeanWindow reserved_mean;
  grown_mean.Init(/*window_ns=*/100, /*capacity=*/1);
  reserved_mean.Init(/*window_ns=*/100, /*capacity=*/16);

  leadlag::MeanStdWindow grown_std;
  leadlag::MeanStdWindow reserved_std;
  grown_std.Init(/*window_ns=*/100, /*capacity=*/1);
  reserved_std.Init(/*window_ns=*/100, /*capacity=*/16);

  for (std::size_t i = 0; i < 8; ++i) {
    const double value = static_cast<double>(i + 1);
    grown_mean.Update(static_cast<std::int64_t>(i), value);
    reserved_mean.Update(static_cast<std::int64_t>(i), value);
    grown_std.Update(static_cast<std::int64_t>(i), value);
    reserved_std.Update(static_cast<std::int64_t>(i), value);
  }

  EXPECT_GT(grown_mean.capacity(), 1U);
  EXPECT_GT(grown_std.capacity(), 1U);
  EXPECT_DOUBLE_EQ(grown_mean.mean(), reserved_mean.mean());
  EXPECT_DOUBLE_EQ(grown_std.mean(), reserved_std.mean());
  EXPECT_DOUBLE_EQ(grown_std.stddev(), reserved_std.stddev());
}

TEST(LeadLagRecordersTest, BboExtremaWindowTracksRollingMinAndMax) {
  leadlag::BboExtremaWindow window;
  window.Init(/*window_ns=*/10, /*capacity=*/2);

  window.Update(Quote(/*event_ns=*/0, /*bid_price=*/100.0,
                      /*ask_price=*/101.0));
  window.Update(Quote(/*event_ns=*/5, /*bid_price=*/99.0,
                      /*ask_price=*/103.0));
  window.Update(Quote(/*event_ns=*/10, /*bid_price=*/102.0,
                      /*ask_price=*/100.5));

  leadlag::BboExtremaSnapshot snapshot = window.snapshot();
  ASSERT_TRUE(snapshot.valid);
  EXPECT_DOUBLE_EQ(snapshot.bid_min, 99.0);
  EXPECT_DOUBLE_EQ(snapshot.bid_max, 102.0);
  EXPECT_DOUBLE_EQ(snapshot.ask_min, 100.5);
  EXPECT_DOUBLE_EQ(snapshot.ask_max, 103.0);

  window.Update(Quote(/*event_ns=*/16, /*bid_price=*/101.0,
                      /*ask_price=*/104.0));
  snapshot = window.snapshot();
  ASSERT_TRUE(snapshot.valid);
  EXPECT_DOUBLE_EQ(snapshot.bid_min, 101.0);
  EXPECT_DOUBLE_EQ(snapshot.bid_max, 102.0);
  EXPECT_DOUBLE_EQ(snapshot.ask_min, 100.5);
  EXPECT_DOUBLE_EQ(snapshot.ask_max, 104.0);
}

TEST(LeadLagRecordersTest, BboExtremaEvictsExpiredBeforePushing) {
  leadlag::RecorderStats stats;
  leadlag::BboExtremaWindow window;
  window.Init(/*window_ns=*/10, /*capacity=*/2, &stats);

  window.Update(Quote(/*event_ns=*/0, /*bid_price=*/100.0,
                      /*ask_price=*/101.0));
  window.Update(Quote(/*event_ns=*/10, /*bid_price=*/101.0,
                      /*ask_price=*/102.0));
  window.Update(Quote(/*event_ns=*/11, /*bid_price=*/102.0,
                      /*ask_price=*/103.0));

  ASSERT_TRUE(window.snapshot().valid);
  EXPECT_EQ(stats.extrema_capacity_grow_count, 0U);
}

TEST(LeadLagRecordersTest, RecorderStateCountsRingQueueCapacityGrow) {
  leadlag::PairConfig pair = PairConfigForRecorder();
  pair.bbo_record.window_ns = 100;
  pair.bbo_record.stats_window_ns = 1000;
  pair.capacity.noise_window_capacity = 1;
  pair.capacity.spread_window_capacity = 1;

  leadlag::RecorderState state;
  state.Init(pair);
  state.SeedActive(Quote(/*event_ns=*/0, /*bid_price=*/100.0,
                         /*ask_price=*/101.0),
                   Quote(/*event_ns=*/0, /*bid_price=*/99.0,
                         /*ask_price=*/100.0));

  [[maybe_unused]] const leadlag::MoveQuantileRoll lead_roll =
      state.OnLeadActiveTick(Quote(/*event_ns=*/1, /*bid_price=*/101.0,
                                   /*ask_price=*/102.0));
  state.OnLagActiveTick(Quote(/*event_ns=*/1, /*bid_price=*/98.5,
                              /*ask_price=*/100.5));

  EXPECT_EQ(state.stats().ring_queue_capacity_grow_count, 5U);
}

TEST(LeadLagRecordersTest, RecorderStateInitResetsCapacityGrowStats) {
  leadlag::PairConfig pair = PairConfigForRecorder();
  pair.bbo_record.window_ns = 100;
  pair.bbo_record.stats_window_ns = 1000;
  pair.capacity.noise_window_capacity = 1;
  pair.capacity.spread_window_capacity = 1;

  leadlag::RecorderState state;
  state.Init(pair);
  state.SeedActive(Quote(/*event_ns=*/0, /*bid_price=*/100.0,
                         /*ask_price=*/101.0),
                   Quote(/*event_ns=*/0, /*bid_price=*/99.0,
                         /*ask_price=*/100.0));
  [[maybe_unused]] const leadlag::MoveQuantileRoll lead_roll =
      state.OnLeadActiveTick(Quote(/*event_ns=*/1, /*bid_price=*/101.0,
                                   /*ask_price=*/102.0));
  state.OnLagActiveTick(Quote(/*event_ns=*/1, /*bid_price=*/98.5,
                              /*ask_price=*/100.5));
  ASSERT_GT(state.stats().ring_queue_capacity_grow_count, 0U);

  state.Init(pair);

  EXPECT_EQ(state.stats().extrema_capacity_grow_count, 0U);
  EXPECT_EQ(state.stats().ring_queue_capacity_grow_count, 0U);
}

TEST(LeadLagRecordersTest, NoiseStateReturnsRollingMeanOfNormalizedStd) {
  leadlag::NoiseState noise;
  noise.Init(/*mid_window_ns=*/100, /*ratio_window_ns=*/1000,
             /*capacity=*/4);

  noise.Update(Quote(/*event_ns=*/0, /*bid_price=*/99.0,
                     /*ask_price=*/101.0));
  noise.Update(Quote(/*event_ns=*/1, /*bid_price=*/101.0,
                     /*ask_price=*/103.0));

  EXPECT_NEAR(noise.value(), 0.0049504950495049506, 1e-15);
}

TEST(LeadLagRecordersTest, SpreadStateTracksMeanAndPositiveBuffer) {
  leadlag::SpreadState spread;
  spread.Init(/*window_ns=*/100, /*capacity=*/2);

  spread.Update(Quote(/*event_ns=*/0, /*bid_price=*/100.0,
                      /*ask_price=*/101.0));
  spread.Update(Quote(/*event_ns=*/1, /*bid_price=*/100.0,
                      /*ask_price=*/103.0));

  EXPECT_DOUBLE_EQ(spread.mean(), 2.0);
  EXPECT_DOUBLE_EQ(spread.buffer(/*current_spread=*/4.5), 2.5);
  EXPECT_DOUBLE_EQ(spread.buffer(/*current_spread=*/1.0), 0.0);
}

TEST(LeadLagRecordersTest, MoveQuantileWindowRollsOnlyAfterBoundary) {
  leadlag::MoveQuantileWindow window;
  window.Init(/*start_ns=*/0, /*stats_window_ns=*/100,
              PairConfigForRecorder().trigger.quantile);
  const leadlag::BboExtremaSnapshot extrema{
      .valid = true,
      .bid_min = 100.0,
      .bid_max = 101.0,
      .ask_min = 103.0,
      .ask_max = 105.0,
  };

  leadlag::MoveQuantileRoll first = window.Update(
      Quote(/*event_ns=*/10, /*bid_price=*/101.0, /*ask_price=*/104.0),
      extrema);
  EXPECT_FALSE(first.rolled);
  EXPECT_EQ(window.sample_count(), 1U);

  leadlag::MoveQuantileRoll boundary = window.Update(
      Quote(/*event_ns=*/100, /*bid_price=*/101.0, /*ask_price=*/104.0),
      extrema);
  EXPECT_FALSE(boundary.rolled);
  EXPECT_EQ(window.sample_count(), 2U);

  leadlag::MoveQuantileRoll rolled = window.Update(
      Quote(/*event_ns=*/101, /*bid_price=*/102.0, /*ask_price=*/103.0),
      extrema);
  ASSERT_TRUE(rolled.rolled);
  EXPECT_EQ(rolled.roll_at_ns, 200);
  EXPECT_DOUBLE_EQ(rolled.up_quantile, 0.02);
  EXPECT_NEAR(rolled.down_quantile, -0.01, 1e-15);
  EXPECT_EQ(window.sample_count(), 1U);
}

TEST(LeadLagRecordersTest, MoveQuantileWindowReturnsZeroForEmptyRolledWindow) {
  leadlag::MoveQuantileWindow window;
  window.Init(/*start_ns=*/0, /*stats_window_ns=*/100,
              PairConfigForRecorder().trigger.quantile);
  const leadlag::BboExtremaSnapshot extrema{
      .valid = true,
      .bid_min = 100.0,
      .bid_max = 101.0,
      .ask_min = 103.0,
      .ask_max = 105.0,
  };

  const leadlag::MoveQuantileRoll rolled = window.Update(
      Quote(/*event_ns=*/101, /*bid_price=*/101.0, /*ask_price=*/104.0),
      extrema);

  ASSERT_TRUE(rolled.rolled);
  EXPECT_DOUBLE_EQ(rolled.up_quantile, 0.0);
  EXPECT_DOUBLE_EQ(rolled.down_quantile, 0.0);
  EXPECT_EQ(window.sample_count(), 1U);
}

TEST(LeadLagRecordersTest, RecorderStateSeedsAndExposesSnapshot) {
  leadlag::RecorderState state;
  state.Init(PairConfigForRecorder());

  state.SeedActive(Quote(/*event_ns=*/0, /*bid_price=*/100.0,
                         /*ask_price=*/101.0),
                   Quote(/*event_ns=*/0, /*bid_price=*/99.0,
                         /*ask_price=*/100.0));
  [[maybe_unused]] const leadlag::MoveQuantileRoll lead_roll =
      state.OnLeadActiveTick(Quote(/*event_ns=*/1, /*bid_price=*/101.0,
                                   /*ask_price=*/102.0));
  state.OnLagActiveTick(Quote(/*event_ns=*/1, /*bid_price=*/98.5,
                              /*ask_price=*/100.5));

  const leadlag::RecorderSnapshot snapshot = state.snapshot();
  ASSERT_TRUE(snapshot.lead_extrema.valid);
  ASSERT_TRUE(snapshot.lag_extrema.valid);
  EXPECT_DOUBLE_EQ(snapshot.lead_extrema.bid_min, 100.0);
  EXPECT_DOUBLE_EQ(snapshot.lead_extrema.bid_max, 101.0);
  EXPECT_DOUBLE_EQ(snapshot.lag_extrema.ask_max, 100.5);
  EXPECT_DOUBLE_EQ(snapshot.lag_spread_mean, 1.5);
  EXPECT_GE(snapshot.lead_noise, 0.0);
  EXPECT_GE(snapshot.lag_noise, 0.0);
}

}  // namespace
