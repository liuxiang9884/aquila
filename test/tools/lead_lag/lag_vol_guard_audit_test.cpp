#include "tools/lead_lag/lag_vol_guard_audit.h"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace aquila::tools::leadlag {
namespace {

BookTicker Ticker(std::int64_t id, std::int64_t exchange_ns, double bid,
                  double ask) {
  return BookTicker{
      .id = id,
      .symbol_id = 4,
      .exchange = Exchange::kGate,
      .exchange_ns = exchange_ns,
      .local_ns = exchange_ns + 1000,
      .bid_price = bid,
      .bid_volume = 1.0,
      .ask_price = ask,
      .ask_volume = 1.0,
  };
}

TEST(LeadLagLagVolGuardAuditTest, DefaultsMatchGoReference) {
  const LagVolGuardAuditConfig config;
  EXPECT_DOUBLE_EQ(config.jump_threshold, 0.005);
  EXPECT_EQ(config.jump_count, 3U);
  EXPECT_EQ(config.jump_window_ns, 300'000'000'000ULL);
  EXPECT_DOUBLE_EQ(config.amplitude_threshold, 0.025);
  EXPECT_EQ(config.amplitude_window_ns, 1'000'000'000ULL);
  EXPECT_EQ(config.cooldown_ns, 900'000'000'000ULL);
}

TEST(LeadLagLagVolGuardAuditTest, TriggersOnJumpCountAndStartsCooldown) {
  LagVolGuardAuditState state;
  state.Init(LagVolGuardAuditConfig{});
  state.OnLagBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 2'000'000'000, 100.7, 100.9));
  state.OnLagBookTicker(Ticker(3, 3'000'000'000, 101.5, 101.7));
  state.OnLagBookTicker(Ticker(4, 4'000'000'000, 102.3, 102.5));

  const LagVolGuardEvaluation eval =
      state.EvaluateAndAdvanceOpenSignal(5'000'000'000);

  EXPECT_TRUE(eval.would_block);
  EXPECT_EQ(eval.reason, LagVolGuardBlockReason::kTrigger);
  EXPECT_EQ(eval.jump_count, 3U);
  EXPECT_TRUE(eval.hot);
  EXPECT_EQ(eval.cooldown_until_ns, 905'000'000'000ULL);
}

TEST(LeadLagLagVolGuardAuditTest, BlocksDuringCooldownWithoutExtendingIt) {
  LagVolGuardAuditState state;
  state.Init(LagVolGuardAuditConfig{});
  state.OnLagBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 2'000'000'000, 100.7, 100.9));
  state.OnLagBookTicker(Ticker(3, 3'000'000'000, 101.5, 101.7));
  state.OnLagBookTicker(Ticker(4, 4'000'000'000, 102.3, 102.5));
  const LagVolGuardEvaluation first =
      state.EvaluateAndAdvanceOpenSignal(5'000'000'000);

  const LagVolGuardEvaluation second =
      state.EvaluateAndAdvanceOpenSignal(6'000'000'000);

  EXPECT_EQ(first.reason, LagVolGuardBlockReason::kTrigger);
  EXPECT_TRUE(second.would_block);
  EXPECT_EQ(second.reason, LagVolGuardBlockReason::kCooldown);
  EXPECT_EQ(second.cooldown_until_ns, first.cooldown_until_ns);
}

TEST(LeadLagLagVolGuardAuditTest, TriggersOnAmplitudeWithoutEnoughJumps) {
  LagVolGuardAuditConfig config;
  config.jump_threshold = 0.50;
  LagVolGuardAuditState state;
  state.Init(config);
  state.OnLagBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 1'100'000'000, 100.1, 100.3));
  state.OnLagBookTicker(Ticker(3, 1'200'000'000, 103.0, 103.2));

  const LagVolGuardEvaluation eval =
      state.EvaluateAndAdvanceOpenSignal(1'300'000'000);

  EXPECT_TRUE(eval.would_block);
  EXPECT_EQ(eval.reason, LagVolGuardBlockReason::kTrigger);
  EXPECT_EQ(eval.jump_count, 0U);
  EXPECT_GT(eval.amplitude, config.amplitude_threshold);
}

TEST(LeadLagLagVolGuardAuditTest, SkipsInvalidAndUnchangedMidUpdates) {
  LagVolGuardAuditState state;
  state.Init(LagVolGuardAuditConfig{});
  state.OnLagBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 2'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(3, 3'000'000'000, -1.0, 100.2));

  const LagVolGuardEvaluation eval =
      state.EvaluateAndAdvanceOpenSignal(4'000'000'000);

  EXPECT_FALSE(eval.would_block);
  EXPECT_EQ(eval.jump_count, 0U);
  EXPECT_DOUBLE_EQ(eval.amplitude, 0.0);
  EXPECT_EQ(state.skipped_update_count(), 1U);
}

TEST(LeadLagLagVolGuardAuditTest, ParsesDurationTextToNanoseconds) {
  std::string error;
  std::uint64_t value = 0;
  EXPECT_TRUE(ParseLagVolGuardAuditDurationNs("5m", &value, &error)) << error;
  EXPECT_EQ(value, 300'000'000'000ULL);
  EXPECT_TRUE(ParseLagVolGuardAuditDurationNs("1500ms", &value, &error))
      << error;
  EXPECT_EQ(value, 1'500'000'000ULL);
  EXPECT_FALSE(ParseLagVolGuardAuditDurationNs("1d", &value, &error));
  EXPECT_NE(error.find("unit must be ns, us, ms, s, m, or h"),
            std::string::npos);
}

}  // namespace
}  // namespace aquila::tools::leadlag
