#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
)toml",
                                      std::string_view trigger_extra = {}) {
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
drift_period = "1m"
drift_min_samples = 20
drift_warmup = "30s"
)toml"} + std::string{trigger_extra} +
         std::string{R"toml(
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

leadlag::ConfigResult LoadCheckedInConfig(
    std::string_view path, const aquila::config::InstrumentCatalog& catalog) {
  return leadlag::LoadConfigFile(SourcePath(path), catalog);
}

TEST(LeadLagConfigTest, LoadsCheckedInConfigWithCatalogMetadata) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result =
      LoadCheckedInConfig("config/strategies/lead_lag.toml", catalog);

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
  EXPECT_TRUE(pair.trigger.drift_guard.enabled);
  EXPECT_DOUBLE_EQ(pair.trigger.drift_guard.drift_instant, 0.015);
  EXPECT_DOUBLE_EQ(pair.trigger.drift_guard.ratio_std, 0.008);
  EXPECT_EQ(pair.trigger.drift_guard.ratio_std_window_ns, 60'000'000'000ULL);
  EXPECT_DOUBLE_EQ(pair.trigger.drift_guard.drift_mean, 0.02);
  EXPECT_EQ(pair.trigger.drift_guard.drift_mean_window_ns, 60'000'000'000ULL);
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
  EXPECT_EQ(pair.execute.open_slippage_ticks, 0U);
  EXPECT_EQ(pair.execute.close_slippage_ticks, 0U);
  EXPECT_EQ(pair.execute.stoploss_slippage_ticks, 0U);
  EXPECT_EQ(pair.execute.close_retry_times, 0U);
  EXPECT_EQ(pair.execute.close_retry_slippage_step_ticks, 0U);
  EXPECT_EQ(pair.execute.parallel, 1U);
  EXPECT_EQ(pair.execute.order_session_fanout, 1U);

  EXPECT_EQ(pair.bbo_record.window_ns, 1'000'000'000ULL);
  EXPECT_EQ(pair.bbo_record.stats_window_ns, 30'000'000'000ULL);
  EXPECT_EQ(pair.capacity.extrema_window_capacity, 16'384U);
  EXPECT_EQ(pair.capacity.move_queue_capacity, 16'384U);
  EXPECT_EQ(pair.capacity.noise_window_capacity, 16'384U);
  EXPECT_EQ(pair.capacity.spread_window_capacity, 16'384U);
  EXPECT_EQ(pair.capacity.drift_guard_window_capacity, 131'072U);

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

  const auto result = LoadCheckedInConfig(
      "config/strategies/lead_lag_first5_20260521.toml", catalog);

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

  const auto result = LoadCheckedInConfig(
      "config/strategies/lead_lag_requested_20260521.toml", catalog);

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

TEST(LeadLagConfigTest, LoadsBitgetRequestedTop30AccountTakerFees) {
  const aquila::config::InstrumentCatalog catalog =
      LoadCatalog("config/instruments/usdt_future_universe.csv");

  const auto result = LoadCheckedInConfig(
      "config/strategies/"
      "lead_lag_bitget_requested_top30_highspeed_fanout4_20260716.toml",
      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::Config& config = result.value;
  constexpr std::array<std::pair<std::string_view, double>, 30> kExpectedFees{{
      {"SKHY_USDT", 0.000065},    {"SNDK_USDT", 0.000065},
      {"SKHYNIX_USDT", 0.000065}, {"SOXL_USDT", 0.000065},
      {"MU_USDT", 0.000065},      {"HYPE_USDT", 0.00015},
      {"ZEC_USDT", 0.0002},       {"KORU_USDT", 0.000065},
      {"SAMSUNG_USDT", 0.000065}, {"DRAM_USDT", 0.000065},
      {"ONDO_USDT", 0.00015},     {"WLD_USDT", 0.0002},
      {"US_USDT", 0.0002},        {"XLM_USDT", 0.0002},
      {"TAO_USDT", 0.00015},      {"NEAR_USDT", 0.0002},
      {"DEXE_USDT", 0.0002},      {"KAITO_USDT", 0.0002},
      {"1000XEC_USDT", 0.0002},   {"BILL_USDT", 0.0002},
      {"0G_USDT", 0.0002},        {"MRVL_USDT", 0.000065},
      {"ZBT_USDT", 0.0002},       {"BCH_USDT", 0.0002},
      {"ENA_USDT", 0.0002},       {"ALLO_USDT", 0.0002},
      {"BSB_USDT", 0.0002},       {"HOME_USDT", 0.0002},
      {"EWY_USDT", 0.000065},     {"SKL_USDT", 0.0002},
  }};

  ASSERT_EQ(config.pairs.size(), kExpectedFees.size());
  for (std::size_t index = 0; index < kExpectedFees.size(); ++index) {
    EXPECT_EQ(config.pairs[index].symbol, kExpectedFees[index].first);
    EXPECT_DOUBLE_EQ(config.pairs[index].lag_taker_fee,
                     kExpectedFees[index].second);
  }
}

TEST(LeadLagConfigTest, LoadsBitgetCombined46AccountTakerFees) {
  const aquila::config::InstrumentCatalog catalog =
      LoadCatalog("config/instruments/usdt_future_universe.csv");

  const auto result = LoadCheckedInConfig(
      "config/strategies/"
      "lead_lag_bitget_combined_46symbols_highspeed_fanout4_20260718.toml",
      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::Config& config = result.value;
  constexpr std::array<std::pair<std::string_view, double>, 46> kExpectedFees{{
      {"SKHY_USDT", 0.000065},    {"SNDK_USDT", 0.000065},
      {"SKHYNIX_USDT", 0.000065}, {"SOXL_USDT", 0.000065},
      {"MU_USDT", 0.000065},      {"HYPE_USDT", 0.00015},
      {"ZEC_USDT", 0.0002},       {"KORU_USDT", 0.000065},
      {"SAMSUNG_USDT", 0.000065}, {"DRAM_USDT", 0.000065},
      {"ONDO_USDT", 0.00015},     {"WLD_USDT", 0.0002},
      {"US_USDT", 0.0002},        {"XLM_USDT", 0.0002},
      {"TAO_USDT", 0.00015},      {"NEAR_USDT", 0.0002},
      {"DEXE_USDT", 0.0002},      {"KAITO_USDT", 0.0002},
      {"1000XEC_USDT", 0.0002},   {"BILL_USDT", 0.0002},
      {"0G_USDT", 0.0002},        {"MRVL_USDT", 0.000065},
      {"ZBT_USDT", 0.0002},       {"BCH_USDT", 0.0002},
      {"ENA_USDT", 0.0002},       {"ALLO_USDT", 0.0002},
      {"BSB_USDT", 0.0002},       {"HOME_USDT", 0.0002},
      {"EWY_USDT", 0.000065},     {"SKL_USDT", 0.0002},
      {"BTC_USDT", 0.00015},      {"SOL_USDT", 0.00015},
      {"DOGE_USDT", 0.00015},     {"XRP_USDT", 0.00015},
      {"TAC_USDT", 0.0002},       {"ORDI_USDT", 0.0002},
      {"SLX_USDT", 0.0002},       {"UB_USDT", 0.0002},
      {"VELVET_USDT", 0.0002},    {"BTW_USDT", 0.0002},
      {"RAVE_USDT", 0.0002},      {"SUI_USDT", 0.00015},
      {"AVAX_USDT", 0.0002},      {"BAS_USDT", 0.0002},
      {"H_USDT", 0.0002},         {"LINK_USDT", 0.0002},
  }};

  ASSERT_EQ(config.pairs.size(), kExpectedFees.size());
  for (std::size_t index = 0; index < kExpectedFees.size(); ++index) {
    const leadlag::PairConfig& pair = config.pairs[index];
    EXPECT_EQ(pair.symbol, kExpectedFees[index].first);
    EXPECT_EQ(pair.lead_exchange, aquila::Exchange::kBinance);
    EXPECT_EQ(pair.lag_exchange, aquila::Exchange::kBitget);
    EXPECT_DOUBLE_EQ(pair.lag_taker_fee, kExpectedFees[index].second);
    EXPECT_EQ(pair.max_lead_freshness_ms, 3);
    EXPECT_EQ(pair.max_lag_freshness_ms, 500);
    EXPECT_DOUBLE_EQ(pair.execute.open_notional, 10.0);
    EXPECT_EQ(pair.execute.parallel, 1U);
    EXPECT_EQ(pair.execute.order_session_fanout, 4U);
  }
}

TEST(LeadLagConfigTest, LoadsCheckedInRequested12SymbolsRiskLimits) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = LoadCheckedInConfig(
      "config/strategies/lead_lag_requested_11symbols_20260522.toml", catalog);

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
    EXPECT_EQ(config.pairs[index].execute.open_slippage_ticks, 2U)
        << config.pairs[index].symbol;
    EXPECT_EQ(config.pairs[index].execute.close_slippage_ticks, 2U)
        << config.pairs[index].symbol;
    EXPECT_EQ(config.pairs[index].execute.stoploss_slippage_ticks, 2U)
        << config.pairs[index].symbol;
  }
}

TEST(LeadLagConfigTest, LoadsCheckedInLabUsdtLiveRiskLimits) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = LoadCheckedInConfig(
      "config/strategies/lead_lag_lab_usdt_20260601.toml", catalog);

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
  EXPECT_EQ(pair.execute.open_slippage_ticks, 500U);
  EXPECT_EQ(pair.execute.close_slippage_ticks, 500U);
  EXPECT_EQ(pair.execute.stoploss_slippage_ticks, 500U);
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

TEST(LeadLagConfigTest, ParsesOrderSessionFanout) {
  const aquila::config::InstrumentCatalog catalog =
      CatalogWithLagQuantityMetadata(1.0, 0);

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", "order_session_fanout = 4\n"), catalog);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.pairs[0].execute.order_session_fanout, 4U);
}

TEST(LeadLagConfigTest, RejectsZeroOrderSessionFanout) {
  const aquila::config::InstrumentCatalog catalog =
      CatalogWithLagQuantityMetadata(1.0, 0);

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", "order_session_fanout = 0\n"), catalog);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("order_session_fanout"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsOrderSessionFanoutAboveMax) {
  const aquila::config::InstrumentCatalog catalog =
      CatalogWithLagQuantityMetadata(1.0, 0);

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", "order_session_fanout = 17\n"), catalog);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("order_session_fanout"), std::string::npos);
}

TEST(LeadLagConfigTest, ParsesExecutionSlippageTicks) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", R"toml(open_slippage_ticks = 7
close_slippage_ticks = 11
stoploss_slippage_ticks = 17
close_retry_times = 2
close_retry_slippage_step_ticks = 3
)toml"),
      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.pairs.size(), 1U);
  EXPECT_EQ(result.value.pairs[0].execute.open_slippage_ticks, 7U);
  EXPECT_EQ(result.value.pairs[0].execute.close_slippage_ticks, 11U);
  EXPECT_EQ(result.value.pairs[0].execute.stoploss_slippage_ticks, 17U);
  EXPECT_EQ(result.value.pairs[0].execute.close_retry_times, 2U);
  EXPECT_EQ(result.value.pairs[0].execute.close_retry_slippage_step_ticks, 3U);
}

TEST(LeadLagConfigTest, ReferenceMigrationDefaultsStayDisabled) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk(""), catalog);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.pairs.size(), 1U);
  const leadlag::PairConfig& pair = result.value.pairs[0];
  EXPECT_EQ(pair.trigger.lag_vol_guard.mode, leadlag::FeatureMode::kOff);
  EXPECT_FALSE(pair.trigger.drift_guard.enabled);
  EXPECT_EQ(pair.execute.taker_buffer.mode, leadlag::FeatureMode::kOff);
  EXPECT_EQ(pair.execute.taker_buffer.source,
            leadlag::GeneratedParamSource::kManual);
  EXPECT_EQ(pair.execute.close_retry_times, 0U);
  EXPECT_EQ(pair.execute.close_retry_slippage_step_ticks, 0U);
}

TEST(LeadLagConfigTest, ParsesGoLikeDriftGuardConfig) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
enabled = false
drift_instant = 0.015
ratio_std = 0.008
ratio_std_window = "1m"
drift_mean = 0.02
drift_mean_window = "1m"
)toml",
                                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::DriftGuardConfig& guard =
      result.value.pairs[0].trigger.drift_guard;
  EXPECT_FALSE(guard.enabled);
  EXPECT_DOUBLE_EQ(guard.drift_instant, 0.015);
  EXPECT_DOUBLE_EQ(guard.ratio_std, 0.008);
  EXPECT_EQ(guard.ratio_std_window_ns, 60'000'000'000ULL);
  EXPECT_DOUBLE_EQ(guard.drift_mean, 0.02);
  EXPECT_EQ(guard.drift_mean_window_ns, 60'000'000'000ULL);
}

TEST(LeadLagConfigTest, RejectsDriftGuardMissingEnabledFirst) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
ratio_std_window = "0s"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("drift_guard.enabled"), std::string::npos);
}

TEST(LeadLagConfigTest, DriftGuardOptionalFieldsDefaultToGoValues) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
enabled = false
)toml",
                                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::DriftGuardConfig& guard =
      result.value.pairs[0].trigger.drift_guard;
  EXPECT_FALSE(guard.enabled);
  EXPECT_DOUBLE_EQ(guard.drift_instant, 0.015);
  EXPECT_DOUBLE_EQ(guard.ratio_std, 0.008);
  EXPECT_EQ(guard.ratio_std_window_ns, 60'000'000'000ULL);
  EXPECT_DOUBLE_EQ(guard.drift_mean, 0.02);
  EXPECT_EQ(guard.drift_mean_window_ns, 60'000'000'000ULL);
}

TEST(LeadLagConfigTest, RejectsEnabledDriftGuardNonPositiveThresholdFirst) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
enabled = true
drift_instant = 0
ratio_std = 0
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("drift_guard.drift_instant"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsEnabledDriftGuardNonFiniteThresholdFirst) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
enabled = true
drift_instant = nan
ratio_std = inf
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("drift_guard.drift_instant"), std::string::npos);
}

TEST(LeadLagConfigTest, RejectsEnabledDriftGuardZeroWindow) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
enabled = true
ratio_std_window = "0s"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("drift_guard.ratio_std_window"),
            std::string::npos);
}

TEST(LeadLagConfigTest, AllowsDisabledDriftGuardNonPositiveThresholds) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
enabled = false
drift_instant = -0.01
ratio_std = 0
ratio_std_window = "1ns"
drift_mean = -1
drift_mean_window = "2ns"
)toml",
                                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::DriftGuardConfig& guard =
      result.value.pairs[0].trigger.drift_guard;
  EXPECT_FALSE(guard.enabled);
  EXPECT_DOUBLE_EQ(guard.drift_instant, -0.01);
  EXPECT_DOUBLE_EQ(guard.ratio_std, 0.0);
  EXPECT_EQ(guard.ratio_std_window_ns, 1ULL);
  EXPECT_DOUBLE_EQ(guard.drift_mean, -1.0);
  EXPECT_EQ(guard.drift_mean_window_ns, 2ULL);
}

TEST(LeadLagConfigTest, RejectsDisabledDriftGuardInvalidDuration) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
enabled = false
ratio_std_window = "0s"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("drift_guard.ratio_std_window"),
            std::string::npos);
}

TEST(LeadLagConfigTest, RejectsDeprecatedDriftLimit) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", "", R"toml(max_lead_freshness_ms = 5
max_lag_freshness_ms = 20
)toml",
                                "drift_limit = 0.02\n"),
      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("drift_limit"), std::string::npos);
  EXPECT_NE(result.error.find("drift_guard.drift_mean"), std::string::npos);
}

TEST(LeadLagConfigTest, ParsesReferenceMigrationTakerBufferShadowConfig) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.execute.taker_buffer]
mode = "shadow"
entry_fixed_pct = 0.0002
normal_close_fixed_pct = 0.0003
exclude_from_cost_model = true
source = "generated"
)toml",
                                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.pairs.size(), 1U);
  const leadlag::PairConfig& pair = result.value.pairs[0];

  EXPECT_EQ(pair.trigger.lag_vol_guard.mode, leadlag::FeatureMode::kOff);
  EXPECT_FALSE(pair.trigger.drift_guard.enabled);
  EXPECT_EQ(pair.execute.close_retry_times, 0U);
  EXPECT_EQ(pair.execute.taker_buffer.mode, leadlag::FeatureMode::kShadow);
  EXPECT_DOUBLE_EQ(pair.execute.taker_buffer.entry_fixed_pct, 0.0002);
  EXPECT_DOUBLE_EQ(pair.execute.taker_buffer.normal_close_fixed_pct, 0.0003);
  EXPECT_TRUE(pair.execute.taker_buffer.exclude_from_cost_model);
  EXPECT_EQ(pair.execute.taker_buffer.source,
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

TEST(LeadLagConfigTest, RejectsDeprecatedNormalCloseRetryAggressive) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", "normal_close_retry_aggressive = true\n"),
      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("normal_close_retry_aggressive"),
            std::string::npos);
}

TEST(LeadLagConfigTest, RejectsDeprecatedExecutionSlippageFields) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result =
      ParseConfigToml(MinimalConfigTomlWithRisk("", R"toml(open_slippage = 7
close_slippage = 11
)toml"),
                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("open_slippage"), std::string::npos);
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

TEST(LeadLagConfigTest, RejectsFreshnessShadowConfig) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.execute.freshness_shadow]
mode = "shadow"
lead_threshold_ms = 8
lag_threshold_ms = 23
source = "generated"
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("freshness_shadow is not supported"),
            std::string::npos);
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

TEST(LeadLagConfigTest, RejectsDeprecatedDriftGuardMode) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
mode = "enforce"
enabled = true
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

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", R"toml(open_slippage_ticks = -1
close_slippage_ticks = 0
stoploss_slippage_ticks = 0
)toml"),
      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("open_slippage_ticks"), std::string::npos);
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
