#include "core/config/data_session_config.h"

#include <filesystem>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "exchange/gate/market_data/data_session_config.h"

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
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

  const aquila::config::InstrumentInfo* binance_btc =
      catalog.Find(aquila::Exchange::kBinance, "BTC_USDT");
  ASSERT_NE(binance_btc, nullptr);
  EXPECT_EQ(binance_btc->symbol_id, 0);
  EXPECT_EQ(binance_btc->symbol, "BTC_USDT");
  EXPECT_EQ(binance_btc->exchange_symbol, "BTCUSDT");
}

TEST(DataSessionConfigTest, BuildsGateFutureMarketDataSessionSettings) {
  const auto config_result = aquila::config::LoadDataSessionConfigFile(
      SourcePath("config/data_sessions/gate_future_market_data.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const auto catalog_result = aquila::config::LoadInstrumentCatalogFromCsv(
      SourcePath(config_result.config.instrument_catalog.file));
  ASSERT_TRUE(catalog_result.ok) << catalog_result.error;

  const auto settings_result =
      aquila::gate::BuildGateFutureMarketDataSessionSettings(
          config_result.config, catalog_result.catalog);
  ASSERT_TRUE(settings_result.ok) << settings_result.error;

  const aquila::gate::GateFutureMarketDataSessionSettings& settings =
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
  EXPECT_EQ(settings.symbols[0].symbol, "BTC_USDT");
  EXPECT_EQ(settings.symbols[0].symbol_id, 0);
  EXPECT_EQ(settings.symbols[1].symbol, "ETH_USDT");
  EXPECT_EQ(settings.symbols[1].symbol_id, 1);
  EXPECT_EQ(settings.symbols[2].symbol, "SOL_USDT");
  EXPECT_EQ(settings.symbols[2].symbol_id, 2);
}

TEST(DataSessionConfigTest, RejectsUnknownGateSubscribeSymbol) {
  const auto config_result = aquila::config::LoadDataSessionConfigFile(
      SourcePath("config/data_sessions/gate_future_market_data.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  auto config = config_result.config;
  config.data_session.subscribe_symbols = {"MISSING_USDT"};

  const auto catalog_result = aquila::config::LoadInstrumentCatalogFromCsv(
      SourcePath(config.instrument_catalog.file));
  ASSERT_TRUE(catalog_result.ok) << catalog_result.error;

  const auto settings_result =
      aquila::gate::BuildGateFutureMarketDataSessionSettings(
          config, catalog_result.catalog);
  ASSERT_FALSE(settings_result.ok);
  EXPECT_NE(settings_result.error.find("MISSING_USDT"), std::string::npos);
}

}  // namespace
