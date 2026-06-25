#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/drift_guard.h"
#include "strategy/lead_lag/types.h"

namespace {

namespace leadlag = aquila::strategy::leadlag;

leadlag::QuoteSnapshot QuoteMid(double mid) {
  return leadlag::QuoteSnapshot{
      .bid_price = mid - 0.5,
      .ask_price = mid + 0.5,
  };
}

leadlag::QuoteSnapshot InvalidQuote() {
  return leadlag::QuoteSnapshot{
      .bid_price = 0.0,
      .ask_price = 0.0,
  };
}

leadlag::DriftGuardConfig EnabledConfig() {
  return leadlag::DriftGuardConfig{
      .enabled = true,
      .drift_instant = 0.015,
      .ratio_std = 0.008,
      .ratio_std_window_ns = 60'000'000'000ULL,
      .drift_mean = 0.02,
      .drift_mean_window_ns = 60'000'000'000ULL,
  };
}

TEST(LeadLagDriftGuardTest, DisabledGuardNeverBlocks) {
  leadlag::DriftGuardConfig config = EnabledConfig();
  config.enabled = false;

  leadlag::DriftGuardState guard;
  guard.Init(config, /*initial_capacity=*/2);
  guard.OnPairedRawBbo(/*event_ns=*/1, QuoteMid(100.0), QuoteMid(102.0));

  const leadlag::DriftGuardSnapshot snapshot =
      guard.Evaluate(config, QuoteMid(100.0), QuoteMid(102.0));

  EXPECT_FALSE(snapshot.enabled);
  EXPECT_FALSE(snapshot.ready);
  EXPECT_FALSE(snapshot.instant_hit);
  EXPECT_FALSE(snapshot.ratio_std_hit);
  EXPECT_FALSE(snapshot.drift_mean_hit);
  EXPECT_FALSE(snapshot.blocked);

  const leadlag::DriftGuardSnapshot enabled_snapshot =
      guard.Evaluate(EnabledConfig(), QuoteMid(100.0), QuoteMid(102.0));
  EXPECT_TRUE(enabled_snapshot.enabled);
  EXPECT_FALSE(enabled_snapshot.ready);
  EXPECT_FALSE(enabled_snapshot.blocked);
}

TEST(LeadLagDriftGuardTest, InvalidQuotesAreNotReadyAndDoNotBlock) {
  const leadlag::DriftGuardConfig config = EnabledConfig();
  leadlag::DriftGuardState guard;
  guard.Init(config, /*initial_capacity=*/2);

  guard.OnPairedRawBbo(/*event_ns=*/1, InvalidQuote(), QuoteMid(102.0));

  leadlag::DriftGuardSnapshot snapshot =
      guard.Evaluate(config, QuoteMid(100.0), QuoteMid(102.0));
  EXPECT_TRUE(snapshot.enabled);
  EXPECT_FALSE(snapshot.ready);
  EXPECT_FALSE(snapshot.blocked);

  guard.OnPairedRawBbo(/*event_ns=*/2, QuoteMid(100.0), QuoteMid(102.0));
  snapshot = guard.Evaluate(config, InvalidQuote(), QuoteMid(102.0));
  EXPECT_TRUE(snapshot.enabled);
  EXPECT_FALSE(snapshot.ready);
  EXPECT_FALSE(snapshot.blocked);
}

TEST(LeadLagDriftGuardTest, InstantRatioBlocksWhenDeviationExceedsThreshold) {
  leadlag::DriftGuardConfig config = EnabledConfig();
  config.ratio_std = 1.0;
  config.drift_mean = 1.0;

  leadlag::DriftGuardState guard;
  guard.Init(config, /*initial_capacity=*/2);
  guard.OnPairedRawBbo(/*event_ns=*/1, QuoteMid(100.0), QuoteMid(102.0));

  const leadlag::DriftGuardSnapshot snapshot =
      guard.Evaluate(config, QuoteMid(100.0), QuoteMid(102.0));

  EXPECT_TRUE(snapshot.enabled);
  EXPECT_TRUE(snapshot.ready);
  EXPECT_NEAR(snapshot.instant_ratio, 1.02, 1e-12);
  EXPECT_TRUE(snapshot.instant_hit);
  EXPECT_FALSE(snapshot.ratio_std_hit);
  EXPECT_FALSE(snapshot.drift_mean_hit);
  EXPECT_TRUE(snapshot.blocked);
}

TEST(LeadLagDriftGuardTest, RatioStdBlocksWhenWindowStdExceedsThreshold) {
  const leadlag::DriftGuardConfig config = EnabledConfig();
  leadlag::DriftGuardState guard;
  guard.Init(config, /*initial_capacity=*/2);

  guard.OnPairedRawBbo(/*event_ns=*/1, QuoteMid(100.0), QuoteMid(100.0));
  guard.OnPairedRawBbo(/*event_ns=*/2, QuoteMid(100.0), QuoteMid(102.0));

  const leadlag::DriftGuardSnapshot snapshot =
      guard.Evaluate(config, QuoteMid(100.0), QuoteMid(100.0));

  EXPECT_TRUE(snapshot.ready);
  EXPECT_FALSE(snapshot.instant_hit);
  EXPECT_NEAR(snapshot.ratio_std, 0.01, 1e-12);
  EXPECT_TRUE(snapshot.ratio_std_hit);
  EXPECT_FALSE(snapshot.drift_mean_hit);
  EXPECT_TRUE(snapshot.blocked);
}

TEST(LeadLagDriftGuardTest,
     DriftMeanBlocksWhenWindowMeanDeviationExceedsThreshold) {
  leadlag::DriftGuardConfig config = EnabledConfig();
  config.drift_instant = 1.0;
  config.ratio_std = 1.0;

  leadlag::DriftGuardState guard;
  guard.Init(config, /*initial_capacity=*/2);
  guard.OnPairedRawBbo(/*event_ns=*/1, QuoteMid(100.0), QuoteMid(103.0));

  const leadlag::DriftGuardSnapshot snapshot =
      guard.Evaluate(config, QuoteMid(100.0), QuoteMid(100.0));

  EXPECT_TRUE(snapshot.ready);
  EXPECT_FALSE(snapshot.instant_hit);
  EXPECT_FALSE(snapshot.ratio_std_hit);
  EXPECT_NEAR(snapshot.drift_mean, 1.03, 1e-12);
  EXPECT_TRUE(snapshot.drift_mean_hit);
  EXPECT_TRUE(snapshot.blocked);
}

TEST(LeadLagDriftGuardTest, RatioStdAndDriftMeanUseIndependentWindows) {
  leadlag::DriftGuardConfig config = EnabledConfig();
  config.drift_instant = 1.0;
  config.ratio_std = 0.001;
  config.ratio_std_window_ns = 10;
  config.drift_mean = 0.01;
  config.drift_mean_window_ns = 100;

  leadlag::DriftGuardState guard;
  guard.Init(config, /*initial_capacity=*/2);
  guard.OnPairedRawBbo(/*event_ns=*/0, QuoteMid(100.0), QuoteMid(100.0));
  guard.OnPairedRawBbo(/*event_ns=*/11, QuoteMid(100.0), QuoteMid(102.0));
  guard.OnPairedRawBbo(/*event_ns=*/12, QuoteMid(100.0), QuoteMid(102.0));

  const leadlag::DriftGuardSnapshot snapshot =
      guard.Evaluate(config, QuoteMid(100.0), QuoteMid(100.0));

  EXPECT_TRUE(snapshot.ready);
  EXPECT_FALSE(snapshot.instant_hit);
  EXPECT_NEAR(snapshot.ratio_std, 0.0, 1e-12);
  EXPECT_FALSE(snapshot.ratio_std_hit);
  EXPECT_NEAR(snapshot.drift_mean, (1.0 + 1.02 + 1.02) / 3.0, 1e-12);
  EXPECT_TRUE(snapshot.drift_mean_hit);
  EXPECT_TRUE(snapshot.blocked);
}

TEST(LeadLagDriftGuardTest, NonMonotonicTimestampDoesNotEvictAllSamples) {
  const leadlag::DriftGuardConfig config = EnabledConfig();
  leadlag::DriftGuardState guard;
  guard.Init(config, /*initial_capacity=*/2);

  guard.OnPairedRawBbo(/*event_ns=*/100, QuoteMid(100.0), QuoteMid(100.0));
  guard.OnPairedRawBbo(/*event_ns=*/105, QuoteMid(100.0), QuoteMid(102.0));
  guard.OnPairedRawBbo(/*event_ns=*/90, QuoteMid(100.0), QuoteMid(102.0));

  const leadlag::DriftGuardSnapshot snapshot =
      guard.Evaluate(config, QuoteMid(100.0), QuoteMid(100.0));

  const double expected_mean = (1.0 + 1.02 + 1.02) / 3.0;
  const double low_delta = 1.0 - expected_mean;
  const double high_delta = 1.02 - expected_mean;
  const double expected_std =
      std::sqrt((low_delta * low_delta + 2.0 * high_delta * high_delta) / 3.0);

  EXPECT_TRUE(snapshot.ready);
  EXPECT_NEAR(snapshot.ratio_std, expected_std, 1e-12);
  EXPECT_TRUE(snapshot.ratio_std_hit);
  EXPECT_FALSE(snapshot.drift_mean_hit);
  EXPECT_TRUE(snapshot.blocked);
}

}  // namespace
