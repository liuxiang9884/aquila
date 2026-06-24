#include "tools/lead_lag/freshness_preflight.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace {

namespace preflight = aquila::tools::leadlag;

aquila::BookTicker Ticker(aquila::Exchange exchange, std::int32_t symbol_id,
                          std::int64_t exchange_ns, std::int64_t latency_ns) {
  return aquila::BookTicker{
      .id = exchange_ns,
      .symbol_id = symbol_id,
      .exchange = exchange,
      .exchange_ns = exchange_ns,
      .local_ns = exchange_ns + latency_ns,
      .bid_price = 1.0,
      .bid_volume = 1.0,
      .ask_price = 1.1,
      .ask_volume = 1.0,
  };
}

TEST(LeadLagFreshnessPreflightTest,
     ComputesGoCompatibleThresholdFromNonNegativeLatencySamples) {
  preflight::FreshnessLatencyAccumulator accumulator;
  accumulator.Observe(
      Ticker(aquila::Exchange::kGate, 4, 1'000'000'000, 1'000'000));
  accumulator.Observe(
      Ticker(aquila::Exchange::kGate, 4, 2'000'000'000, 2'000'000));
  accumulator.Observe(
      Ticker(aquila::Exchange::kGate, 4, 3'000'000'000, 3'000'000));
  accumulator.Observe(Ticker(aquila::Exchange::kGate, 4, 4'000'000'000, -1));

  const std::optional<preflight::FreshnessLatencyStats> stats =
      accumulator.Build();

  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->sample_count, 3U);
  EXPECT_EQ(stats->negative_latency_count, 1U);
  EXPECT_DOUBLE_EQ(stats->mean_ms, 2.0);
  EXPECT_NEAR(stats->std_ms, 0.816496580927726, 1e-12);
  EXPECT_EQ(stats->threshold_ms, 5);
  EXPECT_DOUBLE_EQ(stats->max_ms, 3.0);
}

TEST(LeadLagFreshnessPreflightTest,
     ExtractsLeadLagPairsFromStrategyConfigToml) {
  const toml::table config = toml::parse(R"toml(
[lead_lag]
name = "lead_lag"
version = "1.0"

[[lead_lag.pairs]]
symbol = "SKYAI_USDT"
symbol_id = 4
lead_exchange = "binance"
lag_exchange = "gate"

[[lead_lag.pairs]]
symbol = "PROVE_USDT"
symbol_id = 7
lead_exchange = "kBinance"
lag_exchange = "gate_io"
)toml");

  std::string error;
  const std::optional<std::vector<preflight::FreshnessPairConfig>> pairs =
      preflight::BuildFreshnessPairConfigsFromLeadLagConfig(config, &error);

  ASSERT_TRUE(pairs.has_value()) << error;
  ASSERT_EQ(pairs->size(), 2U);
  EXPECT_EQ((*pairs)[0].symbol_id, 4);
  EXPECT_EQ((*pairs)[0].lead_exchange, aquila::Exchange::kBinance);
  EXPECT_EQ((*pairs)[0].lag_exchange, aquila::Exchange::kGate);
  EXPECT_EQ((*pairs)[1].symbol_id, 7);
  EXPECT_EQ((*pairs)[1].lead_exchange, aquila::Exchange::kBinance);
  EXPECT_EQ((*pairs)[1].lag_exchange, aquila::Exchange::kGate);
}

TEST(LeadLagFreshnessPreflightTest, IgnoresUnconfiguredSymbols) {
  preflight::FreshnessPreflightCollector collector({
      preflight::FreshnessPairConfig{
          .symbol_id = 4,
          .lead_exchange = aquila::Exchange::kBinance,
          .lag_exchange = aquila::Exchange::kGate,
      },
  });

  collector.OnBookTickerAt(
      Ticker(aquila::Exchange::kGate, 7, 100'000'000, 100'000), 100'100'000);
  collector.OnBookTickerAt(
      Ticker(aquila::Exchange::kBinance, 7, 110'000'000, 1'000'000),
      112'000'000);

  EXPECT_TRUE(collector.BuildSummaries().empty());
}

TEST(LeadLagFreshnessPreflightTest,
     SamplesLeadAndLagFreshnessAtLeadDecisionTime) {
  preflight::FreshnessPreflightCollector collector({
      preflight::FreshnessPairConfig{
          .symbol_id = 4,
          .lead_exchange = aquila::Exchange::kBinance,
          .lag_exchange = aquila::Exchange::kGate,
      },
  });

  collector.OnBookTickerAt(
      Ticker(aquila::Exchange::kGate, 4, 100'000'000, 100'000), 100'100'000);
  collector.OnBookTickerAt(
      Ticker(aquila::Exchange::kBinance, 4, 110'000'000, 1'000'000),
      112'000'000);

  const std::vector<preflight::FreshnessGroupSummary> summaries =
      collector.BuildSummaries();

  ASSERT_EQ(summaries.size(), 2U);
  const preflight::FreshnessLatencyStats* lead_stats =
      preflight::FindStats(summaries, 4, aquila::Exchange::kBinance);
  ASSERT_NE(lead_stats, nullptr);
  EXPECT_EQ(lead_stats->sample_count, 1U);
  EXPECT_DOUBLE_EQ(lead_stats->mean_ms, 2.0);

  const preflight::FreshnessLatencyStats* lag_stats =
      preflight::FindStats(summaries, 4, aquila::Exchange::kGate);
  ASSERT_NE(lag_stats, nullptr);
  EXPECT_EQ(lag_stats->sample_count, 1U);
  EXPECT_DOUBLE_EQ(lag_stats->mean_ms, 12.0);
}

TEST(LeadLagFreshnessPreflightTest, SkipsLeadSampleUntilLagSnapshotExists) {
  preflight::FreshnessPreflightCollector collector({
      preflight::FreshnessPairConfig{
          .symbol_id = 4,
          .lead_exchange = aquila::Exchange::kBinance,
          .lag_exchange = aquila::Exchange::kGate,
      },
  });

  collector.OnBookTickerAt(
      Ticker(aquila::Exchange::kBinance, 4, 110'000'000, 1'000'000),
      112'000'000);
  EXPECT_TRUE(collector.BuildSummaries().empty());

  collector.OnBookTickerAt(
      Ticker(aquila::Exchange::kGate, 4, 111'000'000, 100'000), 111'100'000);
  EXPECT_TRUE(collector.BuildSummaries().empty());

  collector.OnBookTickerAt(
      Ticker(aquila::Exchange::kBinance, 4, 120'000'000, 1'000'000),
      123'000'000);

  const std::vector<preflight::FreshnessGroupSummary> summaries =
      collector.BuildSummaries();
  ASSERT_EQ(summaries.size(), 2U);
  const preflight::FreshnessLatencyStats* lag_stats =
      preflight::FindStats(summaries, 4, aquila::Exchange::kGate);
  ASSERT_NE(lag_stats, nullptr);
  EXPECT_EQ(lag_stats->sample_count, 1U);
  EXPECT_DOUBLE_EQ(lag_stats->mean_ms, 12.0);
}

TEST(LeadLagFreshnessPreflightTest, AppliesThresholdsToLeadLagConfigToml) {
  toml::table config = toml::parse(R"toml(
[lead_lag]
name = "lead_lag"
version = "1.0"

[[lead_lag.pairs]]
symbol = "SKYAI_USDT"
symbol_id = 4
lead_exchange = "binance"
lag_exchange = "gate"
max_lead_freshness_ms = 5
max_lag_freshness_ms = 20

[[lead_lag.pairs]]
symbol = "PROVE_USDT"
symbol_id = 7
lead_exchange = "binance"
lag_exchange = "gate"
max_lead_freshness_ms = 5
max_lag_freshness_ms = 20
)toml");
  const std::vector<preflight::FreshnessGroupSummary> summaries{
      preflight::FreshnessGroupSummary{
          .symbol_id = 4,
          .exchange = aquila::Exchange::kBinance,
          .stats = preflight::FreshnessLatencyStats{.sample_count = 10,
                                                    .threshold_ms = 3}},
      preflight::FreshnessGroupSummary{
          .symbol_id = 4,
          .exchange = aquila::Exchange::kGate,
          .stats = preflight::FreshnessLatencyStats{.sample_count = 10,
                                                    .threshold_ms = 8}},
      preflight::FreshnessGroupSummary{
          .symbol_id = 7,
          .exchange = aquila::Exchange::kBinance,
          .stats = preflight::FreshnessLatencyStats{.sample_count = 10,
                                                    .threshold_ms = 4}},
      preflight::FreshnessGroupSummary{
          .symbol_id = 7,
          .exchange = aquila::Exchange::kGate,
          .stats = preflight::FreshnessLatencyStats{.sample_count = 10,
                                                    .threshold_ms = 9}},
  };

  std::string error;
  ASSERT_TRUE(preflight::ApplyFreshnessThresholdsToLeadLagConfig(
      &config, summaries, &error));

  const toml::array* pairs = config["lead_lag"]["pairs"].as_array();
  ASSERT_NE(pairs, nullptr);
  ASSERT_EQ(pairs->size(), 2U);
  const toml::table* first = pairs->get(0)->as_table();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ((*first)["max_lead_freshness_ms"].value<int>(), 3);
  EXPECT_EQ((*first)["max_lag_freshness_ms"].value<int>(), 8);
  const toml::table* second = pairs->get(1)->as_table();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ((*second)["max_lead_freshness_ms"].value<int>(), 4);
  EXPECT_EQ((*second)["max_lag_freshness_ms"].value<int>(), 9);
}

}  // namespace
