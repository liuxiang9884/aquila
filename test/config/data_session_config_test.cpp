#include "exchange/gate/market_data/data_session_config.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/config/instrument_catalog.h"

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

void ExpectOptionalDoubleEq(const std::optional<double>& actual,
                            double expected) {
  ASSERT_TRUE(actual.has_value());
  EXPECT_DOUBLE_EQ(*actual, expected);
}

void ExpectOptionalIntEq(const std::optional<std::int32_t>& actual,
                         std::int32_t expected) {
  ASSERT_TRUE(actual.has_value());
  EXPECT_EQ(*actual, expected);
}

TEST(DataSessionConfigTest, LoadsInstrumentCatalogLookupByExchangeAndSymbol) {
  const auto result = aquila::config::LoadInstrumentCatalogFromCsv(
      SourcePath("config/instruments/usdt_futures.csv"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::InstrumentCatalog& catalog = result.catalog;

  const aquila::config::InstrumentInfo* gate_btc =
      catalog.Find(aquila::Exchange::kGate, "BTC_USDT");
  ASSERT_NE(gate_btc, nullptr);
  EXPECT_EQ(gate_btc->symbol_id, 0);
  EXPECT_EQ(gate_btc->symbol, "BTC_USDT");
  EXPECT_EQ(gate_btc->exchange_symbol, "BTC_USDT");
  EXPECT_EQ(gate_btc->base_asset, "BTC");
  EXPECT_EQ(gate_btc->quote_asset, "USDT");
  EXPECT_EQ(gate_btc->settle_asset, "USDT");
  EXPECT_EQ(gate_btc->product_type, "linear_perpetual");
  EXPECT_EQ(gate_btc->status, "trading");
  EXPECT_EQ(gate_btc->contract_type, "direct");
  EXPECT_DOUBLE_EQ(gate_btc->price_tick, 0.1);
  EXPECT_EQ(gate_btc->price_decimal_places, 1);
  ExpectOptionalDoubleEq(gate_btc->quantity_step, 1.0);
  ExpectOptionalIntEq(gate_btc->quantity_decimal_places, 0);
  EXPECT_DOUBLE_EQ(gate_btc->min_quantity, 1.0);
  EXPECT_DOUBLE_EQ(gate_btc->max_quantity, 100000.0);
  EXPECT_FALSE(gate_btc->max_market_quantity.has_value());
  EXPECT_FALSE(gate_btc->min_notional.has_value());
  EXPECT_DOUBLE_EQ(gate_btc->notional_multiplier, 0.0001);
  ExpectOptionalDoubleEq(gate_btc->price_limit_up, 0.5);
  ExpectOptionalDoubleEq(gate_btc->price_limit_down, 0.5);
  EXPECT_FALSE(gate_btc->market_price_bound.has_value());

  const aquila::config::InstrumentInfo* binance_btc =
      catalog.Find(aquila::Exchange::kBinance, "BTC_USDT");
  ASSERT_NE(binance_btc, nullptr);
  EXPECT_EQ(binance_btc->symbol_id, 0);
  EXPECT_EQ(binance_btc->symbol, "BTC_USDT");
  EXPECT_EQ(binance_btc->exchange_symbol, "BTCUSDT");
  EXPECT_EQ(binance_btc->base_asset, "BTC");
  EXPECT_EQ(binance_btc->quote_asset, "USDT");
  EXPECT_EQ(binance_btc->settle_asset, "USDT");
  EXPECT_EQ(binance_btc->product_type, "linear_perpetual");
  EXPECT_EQ(binance_btc->status, "TRADING");
  EXPECT_EQ(binance_btc->contract_type, "PERPETUAL");
  EXPECT_DOUBLE_EQ(binance_btc->price_tick, 0.10);
  EXPECT_EQ(binance_btc->price_decimal_places, 1);
  ExpectOptionalDoubleEq(binance_btc->quantity_step, 0.001);
  ExpectOptionalIntEq(binance_btc->quantity_decimal_places, 3);
  EXPECT_DOUBLE_EQ(binance_btc->min_quantity, 0.001);
  EXPECT_DOUBLE_EQ(binance_btc->max_quantity, 1000.0);
  ExpectOptionalDoubleEq(binance_btc->max_market_quantity, 120.0);
  ExpectOptionalDoubleEq(binance_btc->min_notional, 100.0);
  EXPECT_DOUBLE_EQ(binance_btc->notional_multiplier, 1.0);
  ExpectOptionalDoubleEq(binance_btc->price_limit_up, 0.05);
  ExpectOptionalDoubleEq(binance_btc->price_limit_down, 0.05);
  ExpectOptionalDoubleEq(binance_btc->market_price_bound, 0.05);
}

TEST(DataSessionConfigTest, BuildsFuturesMarketDataSessionSettings) {
  const auto config_result = aquila::gate::LoadFuturesMarketDataConfigFile(
      SourcePath("config/data_sessions/gate_future_market_data.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const auto catalog_result = aquila::config::LoadInstrumentCatalogFromCsv(
      SourcePath(config_result.config.instrument_catalog.file));
  ASSERT_TRUE(catalog_result.ok) << catalog_result.error;

  const auto settings_result =
      aquila::gate::BuildFuturesMarketDataSessionSettings(
          config_result.config, catalog_result.catalog);
  ASSERT_TRUE(settings_result.ok) << settings_result.error;

  const aquila::gate::FuturesMarketDataSessionSettings& settings =
      settings_result.settings;
  EXPECT_EQ(settings.name, "gate_future_market_data");
  EXPECT_EQ(settings.connection.host, "fx-ws.gateio.ws");
  EXPECT_EQ(settings.connection.service, "443");
  EXPECT_FALSE(settings.connection.enable_tls);
  EXPECT_EQ(settings.connection.target, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  EXPECT_EQ(settings.connection.runtime_policy.io_cpu_id, 2);

  ASSERT_EQ(settings.exchange_symbols.size(), 3u);
  EXPECT_EQ(settings.exchange_symbols[0], "BTC_USDT");
  EXPECT_EQ(settings.exchange_symbols[1], "ETH_USDT");
  EXPECT_EQ(settings.exchange_symbols[2], "SOL_USDT");

  ASSERT_EQ(settings.symbols.size(), 3u);
  EXPECT_EQ(settings.symbols[0].exchange_symbol, "BTC_USDT");
  EXPECT_EQ(settings.symbols[0].symbol_id, 0);
  EXPECT_EQ(settings.symbols[1].exchange_symbol, "ETH_USDT");
  EXPECT_EQ(settings.symbols[1].symbol_id, 1);
  EXPECT_EQ(settings.symbols[2].exchange_symbol, "SOL_USDT");
  EXPECT_EQ(settings.symbols[2].symbol_id, 2);
}

TEST(DataSessionConfigTest, RejectsUnknownGateSubscribeSymbol) {
  const auto config_result = aquila::gate::LoadFuturesMarketDataConfigFile(
      SourcePath("config/data_sessions/gate_future_market_data.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  auto config = config_result.config;
  config.data_session.subscribe_symbols = {"MISSING_USDT"};

  const auto catalog_result = aquila::config::LoadInstrumentCatalogFromCsv(
      SourcePath(config.instrument_catalog.file));
  ASSERT_TRUE(catalog_result.ok) << catalog_result.error;

  const auto settings_result =
      aquila::gate::BuildFuturesMarketDataSessionSettings(
          config, catalog_result.catalog);
  ASSERT_FALSE(settings_result.ok);
  EXPECT_NE(settings_result.error.find("MISSING_USDT"), std::string::npos);
}

}  // namespace
