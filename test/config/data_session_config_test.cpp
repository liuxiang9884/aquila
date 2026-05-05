#include "exchange/gate/market_data/data_session_config.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "exchange/binance/market_data/data_session.h"
#include "exchange/binance/market_data/data_session_config.h"
#include "exchange/gate/market_data/data_session.h"

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
  const aquila::config::InstrumentCatalog& catalog = result.value;

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

TEST(DataSessionConfigTest, LoadsReadyDataSessionConfig) {
  const auto config_result = aquila::gate::LoadDataSessionConfigFile(
      SourcePath("config/data_sessions/gate_data_session.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const aquila::gate::DataSessionConfig& config = config_result.value;
  EXPECT_EQ(config.name, "gate_data_session");
  EXPECT_EQ(config.connection.host, "fx-ws.gateio.ws");
  EXPECT_EQ(config.connection.service, "443");
  EXPECT_TRUE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.target, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 2);

  ASSERT_EQ(config.exchange_symbols.size(), 3u);
  EXPECT_EQ(config.exchange_symbols[0], "BTC_USDT");
  EXPECT_EQ(config.exchange_symbols[1], "ETH_USDT");
  EXPECT_EQ(config.exchange_symbols[2], "SOL_USDT");
  ASSERT_EQ(config.symbol_ids.size(), 3u);
  EXPECT_EQ(config.symbol_ids[0], 0);
  EXPECT_EQ(config.symbol_ids[1], 1);
  EXPECT_EQ(config.symbol_ids[2], 2);

  struct Consumer {
    void OnBookTicker(const aquila::BookTicker&) noexcept {}
  } consumer;

  using Session =
      aquila::gate::DataSession<Consumer,
                                aquila::gate::DefaultPlainWebSocketPolicy,
                                aquila::gate::SessionOnlyDiagnosticsPolicy>;
  Session session(config, consumer);
  EXPECT_EQ(session.name(), "gate_data_session");
  EXPECT_EQ(session.connection().host, "fx-ws.gateio.ws");
  EXPECT_EQ(session.connection().service, "443");
  EXPECT_TRUE(session.connection().enable_tls);
  EXPECT_EQ(session.connection().target, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  EXPECT_EQ(session.connection().runtime_policy.io_cpu_id, 2);

  const std::span<const aquila::gate::SymbolBinding> symbols =
      session.symbols();
  ASSERT_EQ(symbols.size(), 3u);
  EXPECT_EQ(symbols[0].exchange_symbol, "BTC_USDT");
  EXPECT_EQ(symbols[0].symbol_id, 0);
  EXPECT_EQ(symbols[1].exchange_symbol, "ETH_USDT");
  EXPECT_EQ(symbols[1].symbol_id, 1);
  EXPECT_EQ(symbols[2].exchange_symbol, "SOL_USDT");
  EXPECT_EQ(symbols[2].symbol_id, 2);

  EXPECT_EQ(session.phase(), aquila::websocket::ConnectionPhase::kDisconnected);
  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kActive);
  EXPECT_NE(session.last_subscribe_request().find("BTC_USDT"),
            std::string_view::npos);
}

TEST(DataSessionConfigTest, RejectsUnknownGateSubscribeSymbol) {
  const std::string toml_text = std::string{R"toml(
[instrument_catalog]
file = ")toml"} + SourcePath("config/instruments/usdt_futures.csv").string() +
                                R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "gate_data_session"
subscribe_symbols = ["MISSING_USDT"]
settle = "usdt"
wire_format = "sbe"
sbe_schema_id = 1
feed = "book_ticker"

[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"
enable_tls = false

[data_session.websocket.execution_policy]
bind_cpu_id = 2
)toml";
  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::gate::ParseDataSessionConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("MISSING_USDT"), std::string::npos);
}

TEST(DataSessionConfigTest, LoadsReadyBinanceDataSessionConfig) {
  const auto config_result = aquila::binance::LoadDataSessionConfigFile(
      SourcePath("config/data_sessions/binance_data_session.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const aquila::binance::DataSessionConfig& config = config_result.value;
  EXPECT_EQ(config.name, "binance_data_session");
  EXPECT_EQ(config.connection.host, "fstream.binance.com");
  EXPECT_EQ(config.connection.service, "443");
  EXPECT_TRUE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.target,
            "/public/ws/btcusdt@bookTicker/ethusdt@bookTicker/"
            "solusdt@bookTicker");
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 3);

  ASSERT_EQ(config.exchange_symbols.size(), 3u);
  EXPECT_EQ(config.exchange_symbols[0], "BTCUSDT");
  EXPECT_EQ(config.exchange_symbols[1], "ETHUSDT");
  EXPECT_EQ(config.exchange_symbols[2], "SOLUSDT");
  ASSERT_EQ(config.symbol_ids.size(), 3u);
  EXPECT_EQ(config.symbol_ids[0], 0);
  EXPECT_EQ(config.symbol_ids[1], 1);
  EXPECT_EQ(config.symbol_ids[2], 2);

  struct Consumer {
    void OnBookTicker(const aquila::BookTicker&) noexcept {}
  } consumer;

  using Session = aquila::binance::DataSession<
      Consumer, aquila::binance::DefaultTlsWebSocketPolicy,
      aquila::binance::SessionOnlyDiagnosticsPolicy>;
  Session session(config, consumer);
  EXPECT_EQ(session.name(), "binance_data_session");
  EXPECT_EQ(session.connection().host, "fstream.binance.com");
  EXPECT_EQ(session.connection().service, "443");
  EXPECT_TRUE(session.connection().enable_tls);
  EXPECT_EQ(session.connection().target,
            "/public/ws/btcusdt@bookTicker/ethusdt@bookTicker/"
            "solusdt@bookTicker");
  EXPECT_EQ(session.stream_target(),
            "/public/ws/btcusdt@bookTicker/ethusdt@bookTicker/"
            "solusdt@bookTicker");

  const std::span<const aquila::binance::SymbolBinding> symbols =
      session.symbols();
  ASSERT_EQ(symbols.size(), 3u);
  EXPECT_EQ(symbols[0].symbol, "BTCUSDT");
  EXPECT_EQ(symbols[0].symbol_id, 0);
  EXPECT_EQ(symbols[1].symbol, "ETHUSDT");
  EXPECT_EQ(symbols[1].symbol_id, 1);
  EXPECT_EQ(symbols[2].symbol, "SOLUSDT");
  EXPECT_EQ(symbols[2].symbol_id, 2);
}

TEST(DataSessionConfigTest, RejectsUnknownBinanceSubscribeSymbol) {
  const std::string toml_text = std::string{R"toml(
[instrument_catalog]
file = ")toml"} + SourcePath("config/instruments/usdt_futures.csv").string() +
                                R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "binance_data_session"
subscribe_symbols = ["MISSING_USDT"]
market = "um_futures"
feed = "book_ticker"

[data_session.websocket.endpoint]
host = "fstream.binance.com"

[data_session.websocket.execution_policy]
bind_cpu_id = 3
)toml";
  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::binance::ParseDataSessionConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("MISSING_USDT"), std::string::npos);
}

}  // namespace
