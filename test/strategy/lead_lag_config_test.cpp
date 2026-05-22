#include <cstdint>
#include <filesystem>
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

  EXPECT_DOUBLE_EQ(config.pairs[1].lag_instrument.quantity_step, 1.0);
  EXPECT_EQ(config.pairs[1].lag_instrument.quantity_decimal_places, 0);
  EXPECT_DOUBLE_EQ(config.pairs[3].lag_instrument.quantity_step, 1.0);
  EXPECT_EQ(config.pairs[3].lag_instrument.quantity_decimal_places, 0);
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

TEST(LeadLagConfigTest, LoadsCheckedInRequested11SymbolsRiskLimits) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = leadlag::LoadConfigFile(
      SourcePath("config/strategies/lead_lag_requested_11symbols_20260522.toml"),
      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::Config& config = result.value;
  EXPECT_DOUBLE_EQ(config.risk.max_gross_notional, 500.0);
  EXPECT_EQ(config.risk.max_holding_position, 100000);
  ASSERT_EQ(config.pairs.size(), 11U);
}

TEST(LeadLagConfigTest, EntrySpreadLimitFallsBackToTrailingStop) {
  leadlag::ExecuteConfig config{
      .trailing_stop = 0.0125,
      .max_entry_spread = -1.0,
  };

  EXPECT_DOUBLE_EQ(config.EntrySpreadLimit(), 0.0125);
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
