#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "strategy/lead_lag/config.h"

namespace {

namespace leadlag = aquila::strategy::leadlag;

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

aquila::config::InstrumentCatalog LoadCatalog(
    std::string_view path = "config/instruments/usdt_futures.csv") {
  auto result = aquila::config::LoadInstrumentCatalogFromCsv(SourcePath(path));
  EXPECT_TRUE(result.ok) << result.error;
  return std::move(result.value);
}

leadlag::ConfigResult ParseConfigToml(
    std::string_view text, const aquila::config::InstrumentCatalog& catalog) {
  const toml::parse_result parsed = toml::parse(text);
  return leadlag::ParseConfig(parsed, catalog);
}

aquila::config::InstrumentCatalog CatalogWithLagQuantityMetadata(
    std::optional<double> quantity_step,
    std::optional<std::int32_t> quantity_decimal_places) {
  aquila::config::InstrumentCatalog catalog;
  catalog.Add(aquila::config::InstrumentInfo{
      .symbol_id = 0,
      .exchange = aquila::Exchange::kBinance,
      .symbol = "BTC_USDT",
      .exchange_symbol = "BTCUSDT",
      .base_asset = "BTC",
      .quote_asset = "USDT",
      .settle_asset = "USDT",
      .product_type = "linear_perpetual",
      .status = "TRADING",
      .contract_type = "PERPETUAL",
      .price_tick = 0.1,
      .price_decimal_places = 1,
      .quantity_step = 0.001,
      .quantity_decimal_places = 3,
      .min_quantity = 0.001,
      .max_quantity = 1000.0,
      .notional_multiplier = 1.0,
  });
  catalog.Add(aquila::config::InstrumentInfo{
      .symbol_id = 0,
      .exchange = aquila::Exchange::kGate,
      .symbol = "BTC_USDT",
      .exchange_symbol = "BTC_USDT",
      .base_asset = "BTC",
      .quote_asset = "USDT",
      .settle_asset = "USDT",
      .product_type = "linear_perpetual",
      .status = "trading",
      .contract_type = "direct",
      .price_tick = 0.1,
      .price_decimal_places = 1,
      .quantity_step = quantity_step,
      .quantity_decimal_places = quantity_decimal_places,
      .min_quantity = 1.0,
      .max_quantity = 100000.0,
      .notional_multiplier = 0.0001,
  });
  return catalog;
}

std::string MinimalConfigTomlWithRisk(std::string_view risk_section,
                                      std::string_view execute_extra = {},
                                      std::string_view freshness_lines =
                                          R"toml(max_lead_freshness_ms = 5
max_lag_freshness_ms = 20
)toml") {
  return std::string{R"toml(
[lead_lag]
name = "lead_lag"
version = "1.0"

)toml"} + std::string{risk_section} +
         std::string{R"toml(

[[lead_lag.pairs]]
symbol = "BTC_USDT"
symbol_id = 0
lead_exchange = "binance"
lag_exchange = "gate"
lag_taker_fee = 0.00016
)toml"} + std::string{freshness_lines} +
         std::string{R"toml(

[lead_lag.pairs.trigger]
lead = 0.0025
close = 0.0005
lag_part = 0.5
target_profit_rate = 0.0
drift_limit = 0.02
drift_period = "1m"
drift_min_samples = 20
drift_warmup = "30s"

[lead_lag.pairs.trigger.quantile]
move = 0.75
up_min = 0.0
up_max = 0.02
down_min = -0.02
down_max = 0.0
precision = 0.000001

[lead_lag.pairs.execute]
open_notional = 100.0
trailing_stop = 0.01
max_entry_spread = 0.01
)toml"} + std::string{execute_extra} +
         std::string{R"toml(
parallel = 1

[lead_lag.pairs.bbo_record]
window = "1s"
stats_window = "30s"
)toml"};
}

TEST(LeadLagConfigTest, LoadsCheckedInConfigWithCatalogMetadata) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = leadlag::LoadConfigFile(
      SourcePath("config/strategies/lead_lag.toml"), catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::Config& config = result.value;
  EXPECT_EQ(config.name, "lead_lag");
  EXPECT_EQ(config.version, "1.0");
  EXPECT_DOUBLE_EQ(config.risk.max_gross_notional, 0.0);
  EXPECT_EQ(config.risk.max_holding_position, 0);
  ASSERT_EQ(config.pairs.size(), 1U);

  const leadlag::PairConfig& pair = config.pairs.front();
  EXPECT_EQ(pair.symbol, "BTC_USDT");
  EXPECT_EQ(pair.symbol_id, 0);
  EXPECT_EQ(pair.lead_exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(pair.lag_exchange, aquila::Exchange::kGate);
  EXPECT_DOUBLE_EQ(pair.lag_taker_fee, 0.00016);
  EXPECT_EQ(pair.max_lead_freshness_ms, 5);
  EXPECT_EQ(pair.max_lag_freshness_ms, 20);

  EXPECT_DOUBLE_EQ(pair.trigger.lead, 0.0025);
  EXPECT_DOUBLE_EQ(pair.trigger.close, 0.0005);
  EXPECT_DOUBLE_EQ(pair.trigger.lag_part, 0.5);
  EXPECT_DOUBLE_EQ(pair.trigger.target_profit_rate, 0.0);
  EXPECT_DOUBLE_EQ(pair.trigger.drift_limit, 0.02);
  EXPECT_EQ(pair.trigger.drift_period_ns, 60'000'000'000ULL);
  EXPECT_EQ(pair.trigger.drift_min_samples, 20U);
  EXPECT_EQ(pair.trigger.drift_warmup_ns, 30'000'000'000ULL);
  EXPECT_DOUBLE_EQ(pair.trigger.quantile.move, 0.75);
  EXPECT_DOUBLE_EQ(pair.trigger.quantile.up_min, 0.0);
  EXPECT_DOUBLE_EQ(pair.trigger.quantile.up_max, 0.02);
  EXPECT_DOUBLE_EQ(pair.trigger.quantile.down_min, -0.02);
  EXPECT_DOUBLE_EQ(pair.trigger.quantile.down_max, 0.0);
  EXPECT_DOUBLE_EQ(pair.trigger.quantile.precision, 0.000001);
  EXPECT_EQ(pair.trigger.quantile.up_bins, 20'000U);
  EXPECT_EQ(pair.trigger.quantile.down_bins, 20'000U);

  EXPECT_DOUBLE_EQ(pair.execute.open_notional, 100.0);
  EXPECT_DOUBLE_EQ(pair.execute.trailing_stop, 0.01);
  EXPECT_DOUBLE_EQ(pair.execute.max_entry_spread, 0.01);
  EXPECT_DOUBLE_EQ(pair.execute.EntrySpreadLimit(), 0.01);
  EXPECT_EQ(pair.execute.open_slippage, 0U);
  EXPECT_EQ(pair.execute.close_slippage, 0U);
  EXPECT_EQ(pair.execute.parallel, 1U);

  EXPECT_EQ(pair.bbo_record.window_ns, 1'000'000'000ULL);
  EXPECT_EQ(pair.bbo_record.stats_window_ns, 30'000'000'000ULL);
  EXPECT_EQ(pair.capacity.extrema_window_capacity, 16'384U);
  EXPECT_EQ(pair.capacity.move_queue_capacity, 16'384U);
  EXPECT_EQ(pair.capacity.noise_window_capacity, 16'384U);
  EXPECT_EQ(pair.capacity.spread_window_capacity, 16'384U);

  EXPECT_EQ(pair.lag_instrument.symbol_id, 0);
  EXPECT_EQ(pair.lag_instrument.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(pair.lag_instrument.exchange_symbol, "BTC_USDT");
  EXPECT_DOUBLE_EQ(pair.lag_instrument.price_tick, 0.1);
  EXPECT_EQ(pair.lag_instrument.price_decimal_places, 1);
  EXPECT_DOUBLE_EQ(pair.lag_instrument.quantity_step, 1.0);
  EXPECT_EQ(pair.lag_instrument.quantity_decimal_places, 0);
  EXPECT_DOUBLE_EQ(pair.lag_instrument.min_quantity, 1.0);
  EXPECT_DOUBLE_EQ(pair.lag_instrument.max_quantity, 100000.0);
  EXPECT_DOUBLE_EQ(pair.lag_instrument.notional_multiplier, 0.0001);
  EXPECT_DOUBLE_EQ(pair.lag_instrument.lag_taker_fee, 0.00016);
}

TEST(LeadLagConfigTest, LoadsCheckedInFirst5ConfigWithCatalogMetadata) {
  const aquila::config::InstrumentCatalog catalog =
      LoadCatalog("config/instruments/usdt_futures_first5_20260521.csv");

  const auto result = leadlag::LoadConfigFile(
      SourcePath("config/strategies/lead_lag_first5_20260521.toml"), catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::Config& config = result.value;
  ASSERT_EQ(config.pairs.size(), 5U);

  EXPECT_EQ(config.pairs[0].symbol, "PROVE_USDT");
  EXPECT_EQ(config.pairs[1].symbol, "RAVE_USDT");
  EXPECT_EQ(config.pairs[2].symbol, "ZEC_USDT");
  EXPECT_EQ(config.pairs[3].symbol, "SIREN_USDT");
  EXPECT_EQ(config.pairs[4].symbol, "ETC_USDT");

  EXPECT_DOUBLE_EQ(config.pairs[1].lag_instrument.quantity_step, 0.1);
  EXPECT_EQ(config.pairs[1].lag_instrument.quantity_decimal_places, 1);
  EXPECT_DOUBLE_EQ(config.pairs[3].lag_instrument.quantity_step, 0.1);
  EXPECT_EQ(config.pairs[3].lag_instrument.quantity_decimal_places, 1);
}

TEST(LeadLagConfigTest, LoadsCheckedInRequestedConfigWithCatalogMetadata) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = leadlag::LoadConfigFile(
      SourcePath("config/strategies/lead_lag_requested_20260521.toml"),
      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::Config& config = result.value;
  ASSERT_EQ(config.pairs.size(), 8U);

  EXPECT_EQ(config.pairs[0].symbol, "PROVE_USDT");
  EXPECT_EQ(config.pairs[0].symbol_id, 4);
  EXPECT_EQ(config.pairs[1].symbol, "ZEC_USDT");
  EXPECT_EQ(config.pairs[1].symbol_id, 6);
  EXPECT_EQ(config.pairs[2].symbol, "ETC_USDT");
  EXPECT_EQ(config.pairs[2].symbol_id, 8);
  EXPECT_EQ(config.pairs[3].symbol, "DASH_USDT");
  EXPECT_EQ(config.pairs[3].symbol_id, 9);
  EXPECT_EQ(config.pairs[4].symbol, "SUI_USDT");
  EXPECT_EQ(config.pairs[4].symbol_id, 11);
  EXPECT_EQ(config.pairs[5].symbol, "INJ_USDT");
  EXPECT_EQ(config.pairs[5].symbol_id, 12);
  EXPECT_EQ(config.pairs[6].symbol, "ENA_USDT");
  EXPECT_EQ(config.pairs[6].symbol_id, 13);
  EXPECT_EQ(config.pairs[7].symbol, "BRETT_USDT");
  EXPECT_EQ(config.pairs[7].symbol_id, 14);
}

TEST(LeadLagConfigTest, LoadsCheckedInRequested12SymbolsRiskLimits) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = leadlag::LoadConfigFile(
      SourcePath(
          "config/strategies/lead_lag_requested_11symbols_20260522.toml"),
      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::Config& config = result.value;
  EXPECT_DOUBLE_EQ(config.risk.max_gross_notional, 2000.0);
  EXPECT_EQ(config.risk.max_holding_position, 0);
  ASSERT_EQ(config.pairs.size(), 12U);
  EXPECT_EQ(config.pairs[10].symbol, "BRETT_USDT");
  EXPECT_EQ(config.pairs[10].symbol_id, 14);
  EXPECT_EQ(config.pairs[11].symbol, "ETH_USDT");
  EXPECT_EQ(config.pairs[11].symbol_id, 1);
  EXPECT_DOUBLE_EQ(config.pairs[11].lag_instrument.price_tick, 0.01);
  EXPECT_EQ(config.pairs[11].lag_instrument.price_decimal_places, 2);
  EXPECT_DOUBLE_EQ(config.pairs[11].lag_instrument.quantity_step, 1.0);
  EXPECT_EQ(config.pairs[11].lag_instrument.quantity_decimal_places, 0);
  for (std::size_t index = 0; index < config.pairs.size(); ++index) {
    EXPECT_EQ(config.pairs[index].execute.open_slippage, 2U)
        << config.pairs[index].symbol;
    EXPECT_EQ(config.pairs[index].execute.close_slippage, 2U)
        << config.pairs[index].symbol;
  }
}

TEST(LeadLagConfigTest, LoadsCheckedInLabUsdtLiveRiskLimits) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = leadlag::LoadConfigFile(
      SourcePath("config/strategies/lead_lag_lab_usdt_20260601.toml"), catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::Config& config = result.value;
  EXPECT_DOUBLE_EQ(config.risk.max_gross_notional, 2000.0);
  EXPECT_EQ(config.risk.max_holding_position, 0);
  ASSERT_EQ(config.pairs.size(), 1U);

  const leadlag::PairConfig& pair = config.pairs[0];
  EXPECT_EQ(pair.symbol, "LAB_USDT");
  EXPECT_EQ(pair.symbol_id, 15);
  EXPECT_EQ(pair.lead_exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(pair.lag_exchange, aquila::Exchange::kGate);
  EXPECT_DOUBLE_EQ(pair.execute.open_notional, 200.0);
  EXPECT_DOUBLE_EQ(pair.execute.trailing_stop, 0.01);
  EXPECT_DOUBLE_EQ(pair.execute.max_entry_spread, 0.01);
  EXPECT_EQ(pair.execute.open_slippage, 500U);
  EXPECT_EQ(pair.execute.close_slippage, 500U);
  EXPECT_EQ(pair.execute.parallel, 1U);
  EXPECT_DOUBLE_EQ(pair.lag_instrument.price_tick, 1e-05);
  EXPECT_EQ(pair.lag_instrument.price_decimal_places, 5);
  EXPECT_DOUBLE_EQ(pair.lag_instrument.quantity_step, 0.1);
  EXPECT_EQ(pair.lag_instrument.quantity_decimal_places, 1);
}

TEST(LeadLagConfigTest, ParsesRiskWithOnlyGrossNotionalLimit) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result =
      ParseConfigToml(MinimalConfigTomlWithRisk(R"toml([lead_lag.risk]
max_gross_notional = 2000.0
)toml"),
                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_DOUBLE_EQ(result.value.risk.max_gross_notional, 2000.0);
  EXPECT_EQ(result.value.risk.max_holding_position, 0);
  EXPECT_TRUE(result.value.risk.GrossNotionalLimitEnabled());
  EXPECT_FALSE(result.value.risk.HoldingPositionLimitEnabled());
}

TEST(LeadLagConfigTest, ParsesRiskWithZeroHoldingPositionLimitDisabled) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result =
      ParseConfigToml(MinimalConfigTomlWithRisk(R"toml([lead_lag.risk]
max_gross_notional = 2000.0
max_holding_position = 0
)toml"),
                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_DOUBLE_EQ(result.value.risk.max_gross_notional, 2000.0);
  EXPECT_EQ(result.value.risk.max_holding_position, 0);
  EXPECT_TRUE(result.value.risk.GrossNotionalLimitEnabled());
  EXPECT_FALSE(result.value.risk.HoldingPositionLimitEnabled());
}

TEST(LeadLagConfigTest, ParsesRequiredPairFreshnessGuardDurations) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result =
      ParseConfigToml(MinimalConfigTomlWithRisk("", "",
                                                R"toml(max_lead_freshness_ms = 7
max_lag_freshness_ms = 31
)toml"),
                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.pairs.size(), 1U);
  EXPECT_EQ(result.value.pairs[0].max_lead_freshness_ms, 7);
  EXPECT_EQ(result.value.pairs[0].max_lag_freshness_ms, 31);
}

TEST(LeadLagConfigTest, RejectsPairWithoutLeadFreshnessGuardMs) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", "", "max_lag_freshness_ms = 20\n"),
      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("lead_lag.pairs[0].max_lead_freshness_ms"),
            std::string::npos);
}

TEST(LeadLagConfigTest, RejectsPairWithoutLagFreshnessGuardMs) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", "", "max_lead_freshness_ms = 5\n"),
      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("lead_lag.pairs[0].max_lag_freshness_ms"),
            std::string::npos);
}

TEST(LeadLagConfigTest, RejectsGlobalFreshnessGuardDurations) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk(R"toml(
[lead_lag.freshness]
max_lead_freshness_ms = 7
max_lag_freshness_ms = 31
)toml"),
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("lead_lag.freshness"), std::string::npos);
}

TEST(LeadLagConfigTest, EntrySpreadLimitFallsBackToTrailingStop) {
  leadlag::ExecuteConfig config{
      .trailing_stop = 0.0125,
      .max_entry_spread = -1.0,
  };

  EXPECT_DOUBLE_EQ(config.EntrySpreadLimit(), 0.0125);
}

TEST(LeadLagConfigTest, ParsesExecutionSlippageTicks) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result =
      ParseConfigToml(MinimalConfigTomlWithRisk("", R"toml(open_slippage = 7
close_slippage = 11
)toml"),
                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.pairs.size(), 1U);
  EXPECT_EQ(result.value.pairs[0].execute.open_slippage, 7U);
  EXPECT_EQ(result.value.pairs[0].execute.close_slippage, 11U);
}

TEST(LeadLagConfigTest, ReferenceMigrationDefaultsStayDisabled) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk(""), catalog);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.pairs.size(), 1U);
  const leadlag::PairConfig& pair = result.value.pairs[0];
  EXPECT_EQ(pair.trigger.lag_vol_guard.mode, leadlag::FeatureMode::kOff);
  EXPECT_EQ(pair.trigger.drift_guard.mode, leadlag::FeatureMode::kOff);
  EXPECT_EQ(pair.execute.taker_buffer.mode, leadlag::FeatureMode::kOff);
  EXPECT_EQ(pair.execute.taker_buffer.source,
            leadlag::GeneratedParamSource::kManual);
  EXPECT_EQ(pair.execute.freshness_shadow.mode, leadlag::FeatureMode::kOff);
  EXPECT_EQ(pair.execute.freshness_shadow.source,
            leadlag::GeneratedParamSource::kManual);
  EXPECT_FALSE(pair.execute.normal_close_retry_aggressive);
}

TEST(LeadLagConfigTest, ParsesReferenceMigrationShadowConfig) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.execute.taker_buffer]
mode = "shadow"
entry_fixed_pct = 0.0002
normal_close_fixed_pct = 0.0003
exclude_from_cost_model = true
source = "generated"

[lead_lag.pairs.execute.freshness_shadow]
mode = "shadow"
lead_threshold_ms = 8
lag_threshold_ms = 23
source = "generated"
)toml",
                                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.pairs.size(), 1U);
  const leadlag::PairConfig& pair = result.value.pairs[0];

  EXPECT_EQ(pair.trigger.lag_vol_guard.mode, leadlag::FeatureMode::kOff);
  EXPECT_EQ(pair.trigger.drift_guard.mode, leadlag::FeatureMode::kOff);
  EXPECT_FALSE(pair.execute.normal_close_retry_aggressive);
  EXPECT_EQ(pair.execute.taker_buffer.mode, leadlag::FeatureMode::kShadow);
  EXPECT_DOUBLE_EQ(pair.execute.taker_buffer.entry_fixed_pct, 0.0002);
  EXPECT_DOUBLE_EQ(pair.execute.taker_buffer.normal_close_fixed_pct, 0.0003);
  EXPECT_TRUE(pair.execute.taker_buffer.exclude_from_cost_model);
  EXPECT_EQ(pair.execute.taker_buffer.source,
            leadlag::GeneratedParamSource::kGenerated);

  EXPECT_EQ(pair.execute.freshness_shadow.mode, leadlag::FeatureMode::kShadow);
  EXPECT_EQ(pair.execute.freshness_shadow.lead_threshold_ms, 8);
  EXPECT_EQ(pair.execute.freshness_shadow.lag_threshold_ms, 23);
  EXPECT_EQ(pair.execute.freshness_shadow.source,
            leadlag::GeneratedParamSource::kGenerated);
}

TEST(LeadLagConfigTest, RejectsRuntimeAutoTakerBufferFields) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.execute.taker_buffer]
mode = "shadow"
entry_fixed_pct = 0.0002
normal_close_fixed_pct = 0.0003
auto_warmup = "1m"
source = "generated"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("auto_warmup"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsUnimplementedNormalCloseRetryAggressive) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", "normal_close_retry_aggressive = true\n"),
      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("normal_close_retry_aggressive"),
            std::string::npos);
}

TEST(LeadLagConfigTest, ParsesTakerBufferPctWithoutRangeValidation) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.execute.taker_buffer]
mode = "shadow"
entry_fixed_pct = -0.25
normal_close_fixed_pct = 1.5
source = "generated"
)toml",
                                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.pairs.size(), 1U);
  const leadlag::TakerBufferConfig& buffer =
      result.value.pairs[0].execute.taker_buffer;
  EXPECT_DOUBLE_EQ(buffer.entry_fixed_pct, -0.25);
  EXPECT_DOUBLE_EQ(buffer.normal_close_fixed_pct, 1.5);
}

TEST(LeadLagConfigTest, RejectsFreshnessShadowEnforceMode) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.execute.freshness_shadow]
mode = "enforce"
lead_threshold_ms = 8
lag_threshold_ms = 23
source = "generated"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("freshness_shadow.mode"), std::string::npos);
}

TEST(LeadLagConfigTest, AllowsZeroFreshnessShadowThresholds) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.execute.freshness_shadow]
mode = "shadow"
lead_threshold_ms = 0
lag_threshold_ms = 0
source = "generated"
)toml",
                                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.pairs.size(), 1U);
  const leadlag::FreshnessShadowConfig& freshness =
      result.value.pairs[0].execute.freshness_shadow;
  EXPECT_EQ(freshness.lead_threshold_ms, 0);
  EXPECT_EQ(freshness.lag_threshold_ms, 0);
}

TEST(LeadLagConfigTest, RejectsUnimplementedTakerBufferEnforceMode) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.execute.taker_buffer]
mode = "enforce"
entry_fixed_pct = 0.0002
normal_close_fixed_pct = 0.0003
source = "generated"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("taker_buffer.mode"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsUnimplementedLagVolGuardEnforceMode) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.lag_vol_guard]
mode = "enforce"
jump_threshold = 0.005
jump_count = 3
jump_window = "5m"
amplitude_threshold = 0.025
amplitude_window = "1s"
cooldown = "15m"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("lag_vol_guard.mode"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsUnimplementedLagVolGuardShadowMode) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.lag_vol_guard]
mode = "shadow"
jump_threshold = 0.005
jump_count = 3
jump_window = "5m"
amplitude_threshold = 0.025
amplitude_window = "1s"
cooldown = "15m"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("lag_vol_guard.mode"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsUnimplementedDriftGuardEnforceMode) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
mode = "enforce"
drift_instant = 0.015
ratio_std = 0.008
ratio_std_window = "1m"
drift_mean = 0.02
drift_mean_window = "2m"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("drift_guard.mode"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsUnimplementedDriftGuardShadowMode) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
mode = "shadow"
drift_instant = 0.015
ratio_std = 0.008
ratio_std_window = "1m"
drift_mean = 0.02
drift_mean_window = "2m"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("drift_guard.mode"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsNegativeLagQuantityDecimalPlaces) {
  const aquila::config::InstrumentCatalog catalog =
      CatalogWithLagQuantityMetadata(/*quantity_step=*/0.1,
                                     /*quantity_decimal_places=*/-1);

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk(""), catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("trading metadata"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsNegativeExecutionSlippageTicks) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result =
      ParseConfigToml(MinimalConfigTomlWithRisk("", R"toml(open_slippage = -1
close_slippage = 0
)toml"),
                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("open_slippage"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsDuplicateSymbolId) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(
      R"toml(
[lead_lag]
name = "lead_lag"
version = "1.0"

[[lead_lag.pairs]]
symbol = "BTC_USDT"
symbol_id = 0
lead_exchange = "binance"
lag_exchange = "gate"
lag_taker_fee = 0.00016
max_lead_freshness_ms = 5
max_lag_freshness_ms = 20

[lead_lag.pairs.trigger]
lead = 0.0025
close = 0.0005
lag_part = 0.5
target_profit_rate = 0.0
drift_limit = 0.02
drift_period = "1m"
drift_min_samples = 20
drift_warmup = "30s"

[lead_lag.pairs.trigger.quantile]
move = 0.75
up_min = 0.0
up_max = 0.02
down_min = -0.02
down_max = 0.0
precision = 0.000001

[lead_lag.pairs.execute]
open_notional = 100.0
trailing_stop = 0.01
max_entry_spread = 0.01
parallel = 1

[lead_lag.pairs.bbo_record]
window = "1s"
stats_window = "30s"

[[lead_lag.pairs]]
symbol = "BTC_USDT"
symbol_id = 0
lead_exchange = "binance"
lag_exchange = "gate"
lag_taker_fee = 0.00016
max_lead_freshness_ms = 5
max_lag_freshness_ms = 20

[lead_lag.pairs.trigger]
lead = 0.0025
close = 0.0005
lag_part = 0.5
target_profit_rate = 0.0
drift_limit = 0.02
drift_period = "1m"
drift_min_samples = 20
drift_warmup = "30s"

[lead_lag.pairs.trigger.quantile]
move = 0.75
up_min = 0.0
up_max = 0.02
down_min = -0.02
down_max = 0.0
precision = 0.000001

[lead_lag.pairs.execute]
open_notional = 100.0
trailing_stop = 0.01
max_entry_spread = 0.01
parallel = 1

[lead_lag.pairs.bbo_record]
window = "1s"
stats_window = "30s"
)toml",
      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("symbol_id"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsCatalogSymbolIdMismatch) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(
      R"toml(
[lead_lag]
name = "lead_lag"
version = "1.0"

[[lead_lag.pairs]]
symbol = "BTC_USDT"
symbol_id = 1
lead_exchange = "binance"
lag_exchange = "gate"
lag_taker_fee = 0.00016
max_lead_freshness_ms = 5
max_lag_freshness_ms = 20

[lead_lag.pairs.trigger]
lead = 0.0025
close = 0.0005
lag_part = 0.5
target_profit_rate = 0.0
drift_limit = 0.02
drift_period = "1m"
drift_min_samples = 20
drift_warmup = "30s"

[lead_lag.pairs.trigger.quantile]
move = 0.75
up_min = 0.0
up_max = 0.02
down_min = -0.02
down_max = 0.0
precision = 0.000001

[lead_lag.pairs.execute]
open_notional = 100.0
trailing_stop = 0.01
max_entry_spread = 0.01
parallel = 1

[lead_lag.pairs.bbo_record]
window = "1s"
stats_window = "30s"
)toml",
      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("symbol_id"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsInvalidWindowOrdering) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(
      R"toml(
[lead_lag]
name = "lead_lag"
version = "1.0"

[[lead_lag.pairs]]
symbol = "BTC_USDT"
symbol_id = 0
lead_exchange = "binance"
lag_exchange = "gate"
lag_taker_fee = 0.00016
max_lead_freshness_ms = 5
max_lag_freshness_ms = 20

[lead_lag.pairs.trigger]
lead = 0.0025
close = 0.0005
lag_part = 0.5
target_profit_rate = 0.0
drift_limit = 0.02
drift_period = "1m"
drift_min_samples = 20
drift_warmup = "30s"

[lead_lag.pairs.trigger.quantile]
move = 0.75
up_min = 0.0
up_max = 0.02
down_min = -0.02
down_max = 0.0
precision = 0.000001

[lead_lag.pairs.execute]
open_notional = 100.0
trailing_stop = 0.01
max_entry_spread = 0.01
parallel = 1

[lead_lag.pairs.bbo_record]
window = "30s"
stats_window = "1s"
)toml",
      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("stats_window"), std::string::npos);
}

}  // namespace
