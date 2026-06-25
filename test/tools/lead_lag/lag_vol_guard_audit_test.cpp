#include "tools/lead_lag/lag_vol_guard_audit.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/strategy.h"

namespace aquila::tools::leadlag {
namespace {

BookTicker Ticker(std::int64_t id, std::int64_t exchange_ns, double bid,
                  double ask, Exchange exchange = Exchange::kGate) {
  return BookTicker{
      .id = id,
      .symbol_id = 4,
      .exchange = exchange,
      .exchange_ns = exchange_ns,
      .local_ns = exchange_ns + 1000,
      .bid_price = bid,
      .bid_volume = 1.0,
      .ask_price = ask,
      .ask_volume = 1.0,
  };
}

strategy::leadlag::QuoteSnapshot Quote(std::int64_t id, std::int64_t event_ns,
                                       double mid) {
  return strategy::leadlag::QuoteSnapshot{
      .id = id,
      .event_ns = event_ns,
      .exchange_ns = event_ns,
      .local_ns = event_ns + 1000,
      .bid_price = mid - 0.1,
      .ask_price = mid + 0.1,
  };
}

void EnsureLoggingStarted() {
  static const bool started = [] {
    nova::LogConfig config;
    config.set_console_sink_name("");
    config.set_file_sink_name((std::filesystem::temp_directory_path() /
                               "aquila_lag_vol_guard_audit_test.log")
                                  .string());
    nova::InitializeLogging(config);
    return true;
  }();
  (void)started;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

strategy::leadlag::SignalDiagnostics Diagnostics() {
  return strategy::leadlag::SignalDiagnostics{
      .event_ns = 5'000'000'000,
      .role = strategy::leadlag::PairRole::kLead,
      .lead_raw =
          strategy::leadlag::QuoteSnapshot{
              .id = 7001,
              .event_ns = 5'000'000'000,
              .exchange_ns = 5'000'000'000,
              .local_ns = 5'000'001'000,
              .bid_price = 10.0,
              .ask_price = 10.1,
          },
      .lag =
          strategy::leadlag::QuoteSnapshot{
              .id = 8002,
              .event_ns = 4'999'000'000,
              .exchange_ns = 4'999'000'000,
              .local_ns = 4'999'001'000,
              .bid_price = 9.9,
              .ask_price = 10.0,
          },
  };
}

strategy::leadlag::SignalDiagnostics DiagnosticsWithRawMids(double lead_mid,
                                                            double lag_mid) {
  strategy::leadlag::SignalDiagnostics diagnostics = Diagnostics();
  diagnostics.lead_raw = Quote(7001, 5'000'000'000, lead_mid);
  diagnostics.lag = Quote(8002, 4'999'000'000, lag_mid);
  return diagnostics;
}

strategy::leadlag::SignalDecision OpenLongDecision() {
  return strategy::leadlag::SignalDecision{
      .triggered = true,
      .action = strategy::leadlag::SignalAction::kOpenLong,
      .intent =
          strategy::leadlag::OrderIntent{
              .action = strategy::leadlag::SignalAction::kOpenLong,
              .exchange = Exchange::kGate,
              .symbol_id = 4,
              .side = OrderSide::kBuy,
              .price = 10.0,
              .reduce_only = false,
          },
  };
}

strategy::leadlag::SignalDecision CloseLongDecision() {
  return strategy::leadlag::SignalDecision{
      .triggered = true,
      .action = strategy::leadlag::SignalAction::kCloseLong,
      .intent =
          strategy::leadlag::OrderIntent{
              .action = strategy::leadlag::SignalAction::kCloseLong,
              .exchange = Exchange::kGate,
              .symbol_id = 4,
              .side = OrderSide::kSell,
              .price = 10.0,
              .reduce_only = true,
          },
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

TEST(LeadLagLagVolGuardAuditTest,
     PreservesGoLikeArrivalStateForFutureNonMonotonicTicks) {
  LagVolGuardAuditConfig config;
  config.jump_threshold = 0.001;
  config.jump_count = 2;
  config.jump_window_ns = 10'000'000'000ULL;
  config.amplitude_threshold = 1.0;
  LagVolGuardAuditState state;
  state.Init(config);
  state.OnLagBookTicker(Ticker(1, 10'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 60'000'000'000, 101.0, 101.2));
  state.OnLagBookTicker(Ticker(3, 20'000'000'000, 102.0, 102.2));

  const LagVolGuardEvaluation eval =
      state.EvaluateAndAdvanceOpenSignal(50'000'000'000);

  EXPECT_TRUE(eval.would_block);
  EXPECT_EQ(eval.reason, LagVolGuardBlockReason::kTrigger);
  EXPECT_EQ(eval.jump_count, 2U);
  EXPECT_EQ(state.non_monotonic_event_time_count(), 1U);
}

TEST(LeadLagLagVolGuardAuditTest,
     HugeWindowsAndCooldownUseSaturatingArithmetic) {
  LagVolGuardAuditConfig config;
  config.jump_threshold = 0.001;
  config.jump_count = 1;
  config.jump_window_ns = std::numeric_limits<std::uint64_t>::max();
  config.amplitude_window_ns = std::numeric_limits<std::uint64_t>::max();
  config.cooldown_ns = std::numeric_limits<std::uint64_t>::max() - 10;
  LagVolGuardAuditState state;
  state.Init(config);
  state.OnLagBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 2'000'000'000, 101.0, 101.2));

  const LagVolGuardEvaluation eval =
      state.EvaluateAndAdvanceOpenSignal(20'000'000'000);

  EXPECT_TRUE(eval.would_block);
  EXPECT_EQ(eval.reason, LagVolGuardBlockReason::kTrigger);
  EXPECT_EQ(eval.jump_count, 1U);
  EXPECT_EQ(eval.cooldown_until_ns, std::numeric_limits<std::uint64_t>::max());
}

TEST(LeadLagLagVolGuardAuditTest,
     NonPositiveSignalTimeDoesNotTriggerOrAdvanceCooldown) {
  LagVolGuardAuditConfig config;
  config.jump_threshold = 0.001;
  config.jump_count = 1;
  LagVolGuardAuditState state;
  state.Init(config);
  state.OnLagBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 2'000'000'000, 101.0, 101.2));

  const LagVolGuardEvaluation zero_eval = state.EvaluateAndAdvanceOpenSignal(0);
  const LagVolGuardEvaluation negative_eval =
      state.EvaluateAndAdvanceOpenSignal(-1);

  EXPECT_FALSE(zero_eval.would_block);
  EXPECT_EQ(zero_eval.reason, LagVolGuardBlockReason::kNone);
  EXPECT_FALSE(zero_eval.hot);
  EXPECT_EQ(zero_eval.cooldown_until_ns, 0U);
  EXPECT_FALSE(negative_eval.would_block);
  EXPECT_EQ(negative_eval.reason, LagVolGuardBlockReason::kNone);
  EXPECT_FALSE(negative_eval.hot);
  EXPECT_EQ(negative_eval.cooldown_until_ns, 0U);

  const LagVolGuardEvaluation positive_eval =
      state.EvaluateAndAdvanceOpenSignal(3'000'000'000);
  EXPECT_TRUE(positive_eval.would_block);
  EXPECT_EQ(positive_eval.reason, LagVolGuardBlockReason::kTrigger);
  EXPECT_EQ(positive_eval.cooldown_until_ns, 903'000'000'000ULL);
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

TEST(LeadLagLagVolGuardAuditTest, WriterOutputsHeaderAndOpenSignalRow) {
  EnsureLoggingStarted();
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() / "aquila_lag_vol_guard_audit.csv";
  std::filesystem::remove(output_path);

  LagVolGuardAuditCsvWriter writer;
  std::string error;
  ASSERT_TRUE(writer.Open(output_path, &error)) << error;
  writer.Write(LagVolGuardAuditRow{
      .open_signal_index = 0,
      .symbol = "PROVE_USDT",
      .symbol_id = 4,
      .action = "kOpenLong",
      .side = "kBuy",
      .trigger_exchange_ns = 5'000'000'000,
      .lead_exchange_ns = 5'000'000'000,
      .lag_exchange_ns = 4'999'000'000,
      .signal_lead_id = 7001,
      .signal_lag_id = 8002,
      .raw_price = 10.0,
      .would_block = true,
      .would_block_reason = "lag-vol-guard-trigger",
      .lag_vol_jump_count = 3,
      .lag_vol_amplitude = 0.031,
      .lag_vol_hot = true,
      .lag_vol_cooldown_active = false,
      .lag_vol_cooldown_until_ns = 905'000'000'000ULL,
      .config = LagVolGuardAuditConfig{},
      .drift_instant = 1.01,
      .ratio_std = 0.002,
      .drift_mean = 1.004,
      .drift_guard_outcome = "blocked:instant",
  });
  writer.Write(LagVolGuardAuditRow{
      .open_signal_index = 1,
      .symbol = "PROVE_USDT",
      .symbol_id = 4,
      .action = "kOpenShort",
      .side = "kSell",
      .trigger_exchange_ns = 6'000'000'000,
      .lead_exchange_ns = 6'000'000'000,
      .lag_exchange_ns = 5'999'000'000,
      .signal_lead_id = 7003,
      .signal_lag_id = 8004,
      .raw_price = 11.0,
      .would_block = false,
      .would_block_reason = "none",
      .lag_vol_jump_count = 0,
      .lag_vol_amplitude = 0.0,
      .lag_vol_hot = false,
      .lag_vol_cooldown_active = false,
      .lag_vol_cooldown_until_ns = 0,
      .config = LagVolGuardAuditConfig{},
      .drift_guard_outcome = "disabled",
  });
  writer.Close();

  EXPECT_EQ(ReadFile(output_path),
            "open_signal_index,symbol,symbol_id,action,side,"
            "trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,"
            "signal_lead_id,signal_lag_id,raw_price,would_block,"
            "would_block_reason,lag_vol_jump_count,lag_vol_amplitude,"
            "lag_vol_hot,lag_vol_cooldown_active,lag_vol_cooldown_until_ns,"
            "jump_threshold,jump_count_threshold,jump_window_ns,"
            "amplitude_threshold,amplitude_window_ns,cooldown_ns,"
            "drift_instant,ratio_std,drift_mean,drift_guard_outcome\n"
            "0,PROVE_USDT,4,kOpenLong,kBuy,5000000000,5000000000,"
            "4999000000,7001,8002,10,true,lag-vol-guard-trigger,3,"
            "0.031,true,false,905000000000,0.005,3,300000000000,"
            "0.025,1000000000,900000000000,1.01,0.002,1.004,"
            "blocked:instant\n"
            "1,PROVE_USDT,4,kOpenShort,kSell,6000000000,6000000000,"
            "5999000000,7003,8004,11,false,none,0,0,false,false,0,"
            "0.005,3,300000000000,0.025,1000000000,900000000000,"
            "nan,nan,nan,disabled\n");
}

TEST(LeadLagLagVolGuardAuditTest, CollectorRoutesOnlyConfiguredLagExchange) {
  LagVolGuardAuditCollector collector(
      {LagVolGuardAuditPairConfig{.symbol = "PROVE_USDT",
                                  .symbol_id = 4,
                                  .lag_exchange = Exchange::kGate}},
      LagVolGuardAuditConfig{.jump_threshold = 0.001, .jump_count = 2});
  collector.OnBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  BookTicker binance = Ticker(2, 2'000'000'000, 101.0, 101.2);
  binance.exchange = Exchange::kBinance;
  collector.OnBookTicker(binance);
  collector.OnBookTicker(Ticker(3, 3'000'000'000, 101.0, 101.2));
  collector.OnBookTicker(Ticker(4, 4'000'000'000, 102.0, 102.2));

  LagVolGuardAuditRow row;
  ASSERT_TRUE(collector.BuildOpenSignalRow(
      Ticker(5, 5'000'000'000, 0, 0), OpenLongDecision(), Diagnostics(), &row));

  EXPECT_EQ(row.symbol, "PROVE_USDT");
  EXPECT_EQ(row.symbol_id, 4);
  EXPECT_EQ(row.signal_lag_id, 8002);
  EXPECT_TRUE(row.would_block);
  EXPECT_EQ(row.would_block_reason, "lag-vol-guard-trigger");
}

TEST(LeadLagLagVolGuardAuditTest, CollectorWritesBlockedDriftGuardSnapshot) {
  const strategy::leadlag::DriftGuardConfig drift_guard{
      .enabled = true,
      .drift_instant = 0.001,
      .ratio_std = 100.0,
      .ratio_std_window_ns = 60'000'000'000ULL,
      .drift_mean = 100.0,
      .drift_mean_window_ns = 60'000'000'000ULL,
  };
  LagVolGuardAuditCollector collector(
      {LagVolGuardAuditPairConfig{.symbol = "PROVE_USDT",
                                  .symbol_id = 4,
                                  .lead_exchange = Exchange::kBinance,
                                  .lag_exchange = Exchange::kGate,
                                  .drift_guard = drift_guard}},
      LagVolGuardAuditConfig{});
  collector.OnBookTicker(
      Ticker(1, 1'000'000'000, 99.9, 100.1, Exchange::kBinance));
  collector.OnBookTicker(Ticker(2, 2'000'000'000, 100.9, 101.1));

  LagVolGuardAuditRow row;
  ASSERT_TRUE(collector.BuildOpenSignalRow(
      Ticker(3, 5'000'000'000, 0, 0), OpenLongDecision(),
      DiagnosticsWithRawMids(100.0, 101.0), &row));

  EXPECT_EQ(row.drift_guard_outcome, "blocked:instant");
  EXPECT_NEAR(row.drift_instant, 1.01, 1e-12);
  EXPECT_NEAR(row.ratio_std, 0.0, 1e-12);
  EXPECT_NEAR(row.drift_mean, 1.01, 1e-12);
}

TEST(LeadLagLagVolGuardAuditTest, CollectorMarksEnabledDriftGuardNotReady) {
  LagVolGuardAuditCollector collector(
      {LagVolGuardAuditPairConfig{.symbol = "PROVE_USDT",
                                  .symbol_id = 4,
                                  .lead_exchange = Exchange::kBinance,
                                  .lag_exchange = Exchange::kGate,
                                  .drift_guard =
                                      strategy::leadlag::DriftGuardConfig{
                                          .enabled = true,
                                          .drift_instant = 0.001,
                                          .ratio_std = 100.0,
                                          .drift_mean = 100.0,
                                      }}},
      LagVolGuardAuditConfig{});

  LagVolGuardAuditRow row;
  ASSERT_TRUE(collector.BuildOpenSignalRow(
      Ticker(3, 5'000'000'000, 0, 0), OpenLongDecision(),
      DiagnosticsWithRawMids(100.0, 101.0), &row));

  EXPECT_EQ(row.drift_guard_outcome, "not_ready");
  EXPECT_TRUE(std::isnan(row.drift_instant));
  EXPECT_TRUE(std::isnan(row.ratio_std));
  EXPECT_TRUE(std::isnan(row.drift_mean));
}

TEST(LeadLagLagVolGuardAuditTest, CollectorMarksDisabledDriftGuard) {
  LagVolGuardAuditCollector collector(
      {LagVolGuardAuditPairConfig{
          .symbol = "PROVE_USDT",
          .symbol_id = 4,
          .lead_exchange = Exchange::kBinance,
          .lag_exchange = Exchange::kGate,
          .drift_guard = strategy::leadlag::DriftGuardConfig{}}},
      LagVolGuardAuditConfig{});
  collector.OnBookTicker(
      Ticker(1, 1'000'000'000, 99.9, 100.1, Exchange::kBinance));
  collector.OnBookTicker(Ticker(2, 2'000'000'000, 100.9, 101.1));

  LagVolGuardAuditRow row;
  ASSERT_TRUE(collector.BuildOpenSignalRow(
      Ticker(3, 5'000'000'000, 0, 0), OpenLongDecision(),
      DiagnosticsWithRawMids(100.0, 101.0), &row));

  EXPECT_EQ(row.drift_guard_outcome, "disabled");
  EXPECT_TRUE(std::isnan(row.drift_instant));
  EXPECT_TRUE(std::isnan(row.ratio_std));
  EXPECT_TRUE(std::isnan(row.drift_mean));
}

TEST(LeadLagLagVolGuardAuditTest, CollectorIgnoresNonOpenSignals) {
  LagVolGuardAuditCollector collector(
      {LagVolGuardAuditPairConfig{.symbol = "PROVE_USDT",
                                  .symbol_id = 4,
                                  .lag_exchange = Exchange::kGate}},
      LagVolGuardAuditConfig{});

  LagVolGuardAuditRow row;
  EXPECT_FALSE(collector.BuildOpenSignalRow(Ticker(5, 5'000'000'000, 0, 0),
                                            CloseLongDecision(), Diagnostics(),
                                            &row));
}

TEST(LeadLagLagVolGuardAuditTest, BuildsPairsFromLeadLagConfig) {
  strategy::leadlag::Config config;
  config.pairs.push_back(strategy::leadlag::PairConfig{
      .symbol = "PROVE_USDT",
      .symbol_id = 4,
      .lead_exchange = Exchange::kBinance,
      .lag_exchange = Exchange::kGate,
      .trigger =
          strategy::leadlag::TriggerConfig{
              .drift_guard =
                  strategy::leadlag::DriftGuardConfig{
                      .enabled = true,
                      .drift_instant = 0.001,
                  },
          },
      .capacity =
          strategy::leadlag::CapacityConfig{
              .spread_window_capacity = 1234,
          },
  });
  config.pairs.push_back(strategy::leadlag::PairConfig{
      .symbol = "ORDI_USDT",
      .symbol_id = 7,
      .lead_exchange = Exchange::kGate,
      .lag_exchange = Exchange::kBinance,
  });

  const std::vector<LagVolGuardAuditPairConfig> pairs =
      BuildLagVolGuardAuditPairs(config);

  ASSERT_EQ(pairs.size(), 2U);
  EXPECT_EQ(pairs[0].symbol, "PROVE_USDT");
  EXPECT_EQ(pairs[0].symbol_id, 4);
  EXPECT_EQ(pairs[0].lead_exchange, Exchange::kBinance);
  EXPECT_EQ(pairs[0].lag_exchange, Exchange::kGate);
  EXPECT_TRUE(pairs[0].drift_guard.enabled);
  EXPECT_DOUBLE_EQ(pairs[0].drift_guard.drift_instant, 0.001);
  EXPECT_EQ(pairs[0].drift_guard_initial_capacity, 1234U);
  EXPECT_EQ(pairs[1].symbol, "ORDI_USDT");
  EXPECT_EQ(pairs[1].symbol_id, 7);
  EXPECT_EQ(pairs[1].lead_exchange, Exchange::kGate);
  EXPECT_EQ(pairs[1].lag_exchange, Exchange::kBinance);
  EXPECT_FALSE(pairs[1].drift_guard.enabled);
}

}  // namespace
}  // namespace aquila::tools::leadlag
