#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "strategy/lead_lag/alignment.h"
#include "strategy/lead_lag/raw_market_state.h"
#include "strategy/lead_lag/types.h"

namespace {

namespace leadlag = aquila::strategy::leadlag;

leadlag::QuoteSnapshot Quote(std::int64_t local_ns, double bid_price,
                             double ask_price) {
  return leadlag::QuoteSnapshot{
      .local_ns = local_ns,
      .bid_price = bid_price,
      .ask_price = ask_price,
  };
}

leadlag::AlignmentConfig Config(std::uint64_t warmup_ns) {
  return leadlag::AlignmentConfig{
      .drift_period_ns = 100,
      .stats_window_ns = 200,
      .drift_warmup_ns = warmup_ns,
      .drift_min_samples = 2,
      .initial_capacity = 2,
  };
}

TEST(LeadLagAlignmentTest, TracksDriftMeanStdAndFirstTimestamp) {
  leadlag::AlignmentState alignment;
  alignment.Init(Config(/*warmup_ns=*/10));

  alignment.OnPairedRawBbo(/*now_ns=*/100, Quote(100, 99.0, 101.0),
                           Quote(100, 100.0, 102.0));
  alignment.OnPairedRawBbo(/*now_ns=*/105, Quote(105, 99.0, 101.0),
                           Quote(105, 101.0, 103.0));

  const leadlag::AlignmentSnapshot snapshot = alignment.Snapshot();
  EXPECT_TRUE(snapshot.drift_ready);
  EXPECT_EQ(snapshot.drift_samples, 2U);
  EXPECT_EQ(snapshot.first_paired_drift_ns, 100);
  EXPECT_NEAR(snapshot.drift_mean, 1.015, 1e-12);
  EXPECT_NEAR(snapshot.drift_std, 0.005, 1e-12);
  EXPECT_NEAR(snapshot.drift_std_ema, 0.0025, 1e-12);
  EXPECT_NEAR(snapshot.drift_deviation, std::abs(1.015 - 1.0), 1e-12);
}

TEST(LeadLagAlignmentTest, ActivatesAfterMinSamplesAndWarmup) {
  leadlag::AlignmentState alignment;
  alignment.Init(Config(/*warmup_ns=*/10));

  EXPECT_EQ(alignment.UpdatePhase(/*now_ns=*/99, false, false),
            leadlag::AlignmentPhase::kBootstrap);

  alignment.OnPairedRawBbo(/*now_ns=*/100, Quote(100, 99.0, 101.0),
                           Quote(100, 100.0, 102.0));
  EXPECT_EQ(alignment.UpdatePhase(/*now_ns=*/100, true, true),
            leadlag::AlignmentPhase::kAligning);

  alignment.OnPairedRawBbo(/*now_ns=*/105, Quote(105, 99.0, 101.0),
                           Quote(105, 101.0, 103.0));
  EXPECT_EQ(alignment.UpdatePhase(/*now_ns=*/105, true, true),
            leadlag::AlignmentPhase::kAligning);

  EXPECT_EQ(alignment.UpdatePhase(/*now_ns=*/110, true, true),
            leadlag::AlignmentPhase::kActive);
  EXPECT_EQ(alignment.UpdatePhase(/*now_ns=*/120, true, true),
            leadlag::AlignmentPhase::kActive);
}

TEST(LeadLagAlignmentTest, ZeroWarmupFallsBackToStatsWindow) {
  leadlag::AlignmentConfig config = Config(/*warmup_ns=*/0);
  config.stats_window_ns = 20;
  config.drift_min_samples = 1;

  leadlag::AlignmentState alignment;
  alignment.Init(config);

  alignment.OnPairedRawBbo(/*now_ns=*/100, Quote(100, 99.0, 101.0),
                           Quote(100, 100.0, 102.0));
  EXPECT_EQ(alignment.UpdatePhase(/*now_ns=*/119, true, true),
            leadlag::AlignmentPhase::kAligning);
  EXPECT_EQ(alignment.UpdatePhase(/*now_ns=*/120, true, true),
            leadlag::AlignmentPhase::kActive);
}

TEST(LeadLagAlignmentTest, EnterActiveDriftsLeadSeedAndConsumesResumeLead) {
  leadlag::AlignmentState alignment;
  alignment.Init(Config(/*warmup_ns=*/10));
  alignment.OnPairedRawBbo(/*now_ns=*/100, Quote(100, 100.0, 100.0),
                           Quote(100, 101.0, 101.0));
  alignment.OnPairedRawBbo(/*now_ns=*/110, Quote(110, 100.0, 100.0),
                           Quote(110, 101.0, 101.0));
  ASSERT_EQ(alignment.UpdatePhase(/*now_ns=*/110, true, true),
            leadlag::AlignmentPhase::kActive);

  const leadlag::ActiveSeed seed{
      .lead = Quote(90, 100.0, 101.0),
      .lag = Quote(95, 99.0, 100.0),
      .valid = true,
      .resume_lead_tick = true,
  };

  const leadlag::ActiveTransition transition =
      alignment.EnterActive(/*now_ns=*/110, seed, leadlag::PairRole::kLag);

  ASSERT_TRUE(transition.valid);
  EXPECT_EQ(transition.trigger_role, leadlag::PairRole::kLag);
  EXPECT_TRUE(transition.resume_lead_tick);
  EXPECT_EQ(transition.lead_seed_raw.local_ns, 90);
  EXPECT_EQ(transition.lag_seed.local_ns, 95);
  EXPECT_DOUBLE_EQ(transition.lead_seed_drifted.bid_price, 101.0);
  EXPECT_DOUBLE_EQ(transition.lead_seed_drifted.ask_price, 102.01);

  EXPECT_FALSE(alignment.ConsumeResumeLeadTick(leadlag::PairRole::kLag));
  EXPECT_TRUE(alignment.ConsumeResumeLeadTick(leadlag::PairRole::kLead));
  EXPECT_FALSE(alignment.ConsumeResumeLeadTick(leadlag::PairRole::kLead));
  EXPECT_FALSE(alignment.Snapshot().resume_lead_tick);
}

}  // namespace
