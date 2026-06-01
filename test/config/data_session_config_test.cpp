#include "exchange/gate/market_data/data_session_config.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "core/market_data/data_shm_config.h"
#include "exchange/binance/market_data/data_session.h"
#include "exchange/binance/market_data/data_session_config.h"
#include "exchange/gate/market_data/data_session.h"
#include "nova/utils/log.h"

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

  const aquila::config::InstrumentInfo* gate_rave =
      catalog.Find(aquila::Exchange::kGate, "RAVE_USDT");
  ASSERT_NE(gate_rave, nullptr);
  ExpectOptionalDoubleEq(gate_rave->quantity_step, 0.1);
  ExpectOptionalIntEq(gate_rave->quantity_decimal_places, 1);
  EXPECT_DOUBLE_EQ(gate_rave->min_quantity, 0.1);

  const aquila::config::InstrumentInfo* gate_lab =
      catalog.Find(aquila::Exchange::kGate, "LAB_USDT");
  ASSERT_NE(gate_lab, nullptr);
  EXPECT_EQ(gate_lab->symbol_id, 15);
  EXPECT_EQ(gate_lab->symbol, "LAB_USDT");
  EXPECT_EQ(gate_lab->exchange_symbol, "LAB_USDT");
  EXPECT_EQ(gate_lab->base_asset, "LAB");
  EXPECT_EQ(gate_lab->quote_asset, "USDT");
  EXPECT_EQ(gate_lab->settle_asset, "USDT");
  EXPECT_EQ(gate_lab->product_type, "linear_perpetual");
  EXPECT_EQ(gate_lab->status, "TRADING");
  EXPECT_EQ(gate_lab->contract_type, "direct");
  EXPECT_DOUBLE_EQ(gate_lab->price_tick, 1e-05);
  EXPECT_EQ(gate_lab->price_decimal_places, 5);
  ExpectOptionalDoubleEq(gate_lab->quantity_step, 0.1);
  ExpectOptionalIntEq(gate_lab->quantity_decimal_places, 1);
  EXPECT_DOUBLE_EQ(gate_lab->min_quantity, 0.1);
  EXPECT_DOUBLE_EQ(gate_lab->max_quantity, 1200.0);
  ExpectOptionalDoubleEq(gate_lab->max_market_quantity, 800.0);
  EXPECT_FALSE(gate_lab->min_notional.has_value());
  EXPECT_DOUBLE_EQ(gate_lab->notional_multiplier, 100.0);
  ExpectOptionalDoubleEq(gate_lab->price_limit_up, 0.2);
  ExpectOptionalDoubleEq(gate_lab->price_limit_down, 0.2);
  ExpectOptionalDoubleEq(gate_lab->market_price_bound, 0.05);

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

  const aquila::config::InstrumentInfo* binance_lab =
      catalog.Find(aquila::Exchange::kBinance, "LAB_USDT");
  ASSERT_NE(binance_lab, nullptr);
  EXPECT_EQ(binance_lab->symbol_id, 15);
  EXPECT_EQ(binance_lab->symbol, "LAB_USDT");
  EXPECT_EQ(binance_lab->exchange_symbol, "LABUSDT");
  EXPECT_EQ(binance_lab->base_asset, "LAB");
  EXPECT_EQ(binance_lab->quote_asset, "USDT");
  EXPECT_EQ(binance_lab->settle_asset, "USDT");
  EXPECT_EQ(binance_lab->product_type, "linear_perpetual");
  EXPECT_EQ(binance_lab->status, "TRADING");
  EXPECT_EQ(binance_lab->contract_type, "PERPETUAL");
  EXPECT_DOUBLE_EQ(binance_lab->price_tick, 0.0001);
  EXPECT_EQ(binance_lab->price_decimal_places, 4);
  ExpectOptionalDoubleEq(binance_lab->quantity_step, 1.0);
  ExpectOptionalIntEq(binance_lab->quantity_decimal_places, 0);
  EXPECT_DOUBLE_EQ(binance_lab->min_quantity, 1.0);
  EXPECT_DOUBLE_EQ(binance_lab->max_quantity, 6000000.0);
  ExpectOptionalDoubleEq(binance_lab->max_market_quantity, 600000.0);
  ExpectOptionalDoubleEq(binance_lab->min_notional, 5.0);
  EXPECT_DOUBLE_EQ(binance_lab->notional_multiplier, 1.0);
  ExpectOptionalDoubleEq(binance_lab->price_limit_up, 0.15);
  ExpectOptionalDoubleEq(binance_lab->price_limit_down, 0.15);
  ExpectOptionalDoubleEq(binance_lab->market_price_bound, 0.15);
}

TEST(DataSessionConfigTest, LoadsReadyDataSessionConfig) {
  const auto config_result = aquila::gate::LoadDataSessionConfigFile(
      SourcePath("config/data_sessions/gate_data_session.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const aquila::gate::DataSessionConfig& config = config_result.value;
  EXPECT_EQ(config.name, "gate_data_session");
  EXPECT_EQ(config.connection.host, "fx-ws.gateio.ws");
  EXPECT_EQ(config.connection.port, "443");
  EXPECT_TRUE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.target, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 2);
  EXPECT_TRUE(config.connection.extra_headers.empty());

  ASSERT_EQ(config.exchange_symbols.size(), 3u);
  EXPECT_EQ(config.exchange_symbols[0], "BTC_USDT");
  EXPECT_EQ(config.exchange_symbols[1], "ETH_USDT");
  EXPECT_EQ(config.exchange_symbols[2], "SOL_USDT");
  ASSERT_EQ(config.symbol_ids.size(), 3u);
  EXPECT_EQ(config.symbol_ids[0], 0);
  EXPECT_EQ(config.symbol_ids[1], 1);
  EXPECT_EQ(config.symbol_ids[2], 2);
  EXPECT_TRUE(config.book_ticker_shm.enabled);
  EXPECT_EQ(config.book_ticker_shm.shm_name, "aquila_gate_market_data");
  EXPECT_EQ(config.book_ticker_shm.channel_name, "book_ticker_channel");
  EXPECT_TRUE(config.book_ticker_shm.create);
  EXPECT_FALSE(config.book_ticker_shm.remove_existing);

  struct DataSink {
    void OnBookTicker(const aquila::BookTicker&) noexcept {}
  } data_sink;

  using Session =
      aquila::gate::DataSession<DataSink,
                                aquila::gate::DefaultPlainWebSocketPolicy,
                                aquila::gate::SessionOnlyDiagnosticsPolicy>;
  Session session(config, data_sink);
  EXPECT_EQ(session.name(), "gate_data_session");
  EXPECT_EQ(session.connection().host, "fx-ws.gateio.ws");
  EXPECT_EQ(session.connection().port, "443");
  EXPECT_TRUE(session.connection().enable_tls);
  EXPECT_EQ(session.connection().target, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  EXPECT_EQ(session.connection().runtime_policy.io_cpu_id, 2);
  EXPECT_TRUE(session.connection().extra_headers.empty());

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

TEST(DataSessionConfigTest, LoadsReadyFirst5GateDataSessionConfig) {
  const auto config_result = aquila::gate::LoadDataSessionConfigFile(SourcePath(
      "config/data_sessions/gate_data_session_first5_20260521.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const aquila::gate::DataSessionConfig& config = config_result.value;
  EXPECT_EQ(config.name, "gate_data_session_first5");
  ASSERT_EQ(config.exchange_symbols.size(), 5u);
  EXPECT_EQ(config.exchange_symbols[0], "PROVE_USDT");
  EXPECT_EQ(config.exchange_symbols[1], "RAVE_USDT");
  EXPECT_EQ(config.exchange_symbols[2], "ZEC_USDT");
  EXPECT_EQ(config.exchange_symbols[3], "SIREN_USDT");
  EXPECT_EQ(config.exchange_symbols[4], "ETC_USDT");
  EXPECT_EQ(config.book_ticker_shm.shm_name,
            "aquila_gate_market_data_first5_20260521");
  EXPECT_TRUE(config.book_ticker_shm.remove_existing);
}

TEST(DataSessionConfigTest, LoadsReadyRequestedGateDataSessionConfig) {
  const auto config_result = aquila::gate::LoadDataSessionConfigFile(SourcePath(
      "config/data_sessions/gate_data_session_requested_20260521.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const aquila::gate::DataSessionConfig& config = config_result.value;
  EXPECT_EQ(config.name, "gate_data_session_requested");
  ASSERT_EQ(config.exchange_symbols.size(), 12u);
  EXPECT_EQ(config.exchange_symbols[0], "PROVE_USDT");
  EXPECT_EQ(config.exchange_symbols[1], "RAVE_USDT");
  EXPECT_EQ(config.exchange_symbols[2], "ZEC_USDT");
  EXPECT_EQ(config.exchange_symbols[3], "SIREN_USDT");
  EXPECT_EQ(config.exchange_symbols[4], "ETC_USDT");
  EXPECT_EQ(config.exchange_symbols[5], "DASH_USDT");
  EXPECT_EQ(config.exchange_symbols[6], "RIVER_USDT");
  EXPECT_EQ(config.exchange_symbols[7], "SUI_USDT");
  EXPECT_EQ(config.exchange_symbols[8], "INJ_USDT");
  EXPECT_EQ(config.exchange_symbols[9], "ENA_USDT");
  EXPECT_EQ(config.exchange_symbols[10], "BRETT_USDT");
  EXPECT_EQ(config.exchange_symbols[11], "ETH_USDT");
  EXPECT_EQ(config.symbol_ids[0], 4);
  EXPECT_EQ(config.symbol_ids[10], 14);
  EXPECT_EQ(config.symbol_ids[11], 1);
  EXPECT_EQ(config.book_ticker_shm.shm_name,
            "aquila_gate_market_data_requested_20260521");
  EXPECT_TRUE(config.book_ticker_shm.remove_existing);
}

TEST(DataSessionConfigTest,
     LoadsReadyLabUsdtPrivatePlainGateDataSessionConfig) {
  const auto config_result = aquila::gate::LoadDataSessionConfigFile(SourcePath(
      "config/data_sessions/"
      "gate_data_session_lab_usdt_private_plain_20260601.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const aquila::gate::DataSessionConfig& config = config_result.value;
  EXPECT_EQ(config.name, "gate_data_session_lab_usdt_private_plain_20260601");
  EXPECT_EQ(config.connection.host, "fxws-private.gateapi.io");
  EXPECT_EQ(config.connection.connect_ip, "10.0.1.154");
  EXPECT_EQ(config.connection.port, "80");
  EXPECT_FALSE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.target, "/v4/ws/usdt/sbe?sbe_schema_id=1");
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 2);

  ASSERT_EQ(config.exchange_symbols.size(), 1u);
  EXPECT_EQ(config.exchange_symbols[0], "LAB_USDT");
  ASSERT_EQ(config.symbol_ids.size(), 1u);
  EXPECT_EQ(config.symbol_ids[0], 15);
  EXPECT_TRUE(config.book_ticker_shm.enabled);
  EXPECT_EQ(config.book_ticker_shm.shm_name,
            "aquila_gate_market_data_lab_usdt_20260601");
  EXPECT_TRUE(config.book_ticker_shm.create);
  EXPECT_TRUE(config.book_ticker_shm.remove_existing);
}

TEST(DataSessionConfigTest, LoadsGateLogConfigFromToml) {
  const toml::table toml = toml::parse_file(
      SourcePath("config/data_sessions/gate_data_session.toml").string());

  nova::LogConfig log_config;
  log_config.FromToml(toml["log"]);

  EXPECT_EQ(log_config.log_level(), nova::LogLevel::kLogInfo);
  EXPECT_EQ(log_config.file_sink_name(),
            "/home/liuxiang/log/gate_data_session.log");
  EXPECT_EQ(log_config.console_sink_name(), "gate_data_session_console");
  EXPECT_EQ(log_config.backend_thread_name(), "gate_data_session_log");
  EXPECT_EQ(log_config.backend_cpu_affinity(), 5);
  EXPECT_EQ(log_config.format_pattern(),
            "%(log_level_short_code)%(time) %(process_id):%(thread_id) "
            "%(file_name):%(caller_function):%(line_number)] %(message)");
  EXPECT_EQ(log_config.timestamp_pattern(), "%Y-%m-%d %H:%M:%S.%Qns");
}

TEST(DataSessionConfigTest, ParsesGateDataSessionFromAlreadyParsedToml) {
  const std::filesystem::path config_path =
      SourcePath("config/data_sessions/gate_data_session.toml");
  const toml::table toml = toml::parse_file(config_path.string());

  const auto config_result =
      aquila::gate::ParseDataSessionConfig(toml, config_path);
  ASSERT_TRUE(config_result.ok) << config_result.error;

  EXPECT_EQ(config_result.value.name, "gate_data_session");
  EXPECT_EQ(config_result.value.connection.target,
            "/v4/ws/usdt/sbe?sbe_schema_id=1");
  ASSERT_EQ(config_result.value.exchange_symbols.size(), 3u);
  EXPECT_EQ(config_result.value.exchange_symbols[0], "BTC_USDT");
  EXPECT_EQ(config_result.value.symbol_ids[0], 0);
}

TEST(DataSessionConfigTest, ParsesGateDataShmSinkConfig) {
  const std::string toml_text = std::string{R"toml(
[instrument_catalog]
file = ")toml"} + SourcePath("config/instruments/usdt_futures.csv").string() +
                                R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "gate_data_session"
subscribe_symbols = ["BTC_USDT"]
settle = "usdt"
wire_format = "sbe"
sbe_schema_id = 1
feed = "book_ticker"

[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"
enable_tls = false

[data_session.websocket.execution_policy]
bind_cpu_id = 2

[data_shm_sink]
enabled = true
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
create = true
remove_existing = false
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::gate::ParseDataSessionConfig(parsed);
  ASSERT_TRUE(result.ok) << result.error;

  const aquila::market_data::BookTickerShmConfig& shm =
      result.value.book_ticker_shm;
  EXPECT_TRUE(shm.enabled);
  EXPECT_EQ(shm.shm_name, "aquila_gate_market_data");
  EXPECT_EQ(shm.channel_name, "book_ticker_channel");
  EXPECT_TRUE(shm.create);
  EXPECT_FALSE(shm.remove_existing);
}

TEST(DataSessionConfigTest, LoadsBinanceLogConfigFromToml) {
  const toml::table toml = toml::parse_file(
      SourcePath("config/data_sessions/binance_data_session.toml").string());

  nova::LogConfig log_config;
  log_config.FromToml(toml["log"]);

  EXPECT_EQ(log_config.log_level(), nova::LogLevel::kLogInfo);
  EXPECT_EQ(log_config.file_sink_name(),
            "/home/liuxiang/log/binance_data_session.log");
  EXPECT_EQ(log_config.console_sink_name(), "binance_data_session_console");
  EXPECT_EQ(log_config.backend_thread_name(), "binance_data_session_log");
  EXPECT_EQ(log_config.backend_cpu_affinity(), 5);
  EXPECT_EQ(log_config.format_pattern(),
            "%(log_level_short_code)%(time) %(process_id):%(thread_id) "
            "%(file_name):%(caller_function):%(line_number)] %(message)");
  EXPECT_EQ(log_config.timestamp_pattern(), "%Y-%m-%d %H:%M:%S.%Qns");
}

TEST(DataSessionConfigTest, ParsesBinanceDataSessionFromAlreadyParsedToml) {
  const std::filesystem::path config_path =
      SourcePath("config/data_sessions/binance_data_session.toml");
  const toml::table toml = toml::parse_file(config_path.string());

  const auto config_result =
      aquila::binance::ParseDataSessionConfig(toml, config_path);
  ASSERT_TRUE(config_result.ok) << config_result.error;

  EXPECT_EQ(config_result.value.name, "binance_data_session");
  EXPECT_EQ(config_result.value.connection.target,
            "/public/ws/btcusdt@bookTicker/ethusdt@bookTicker/"
            "solusdt@bookTicker");
  EXPECT_TRUE(config_result.value.connection.extra_headers.empty());
  ASSERT_EQ(config_result.value.exchange_symbols.size(), 3u);
  EXPECT_EQ(config_result.value.exchange_symbols[0], "BTCUSDT");
  EXPECT_EQ(config_result.value.symbol_ids[0], 0);
}

TEST(DataSessionConfigTest, ParsesBinanceDataShmSinkConfig) {
  const std::string toml_text = std::string{R"toml(
[instrument_catalog]
file = ")toml"} + SourcePath("config/instruments/usdt_futures.csv").string() +
                                R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "binance_data_session"
subscribe_symbols = ["BTC_USDT"]
market = "um_futures"
feed = "book_ticker"

[data_session.websocket.endpoint]
host = "fstream.binance.com"

[data_session.websocket.execution_policy]
bind_cpu_id = 3

[data_shm_sink]
enabled = true
shm_name = "aquila_binance_market_data"
channel_name = "book_ticker_channel"
create = true
remove_existing = true
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::binance::ParseDataSessionConfig(parsed);
  ASSERT_TRUE(result.ok) << result.error;

  const aquila::market_data::BookTickerShmConfig& shm =
      result.value.book_ticker_shm;
  EXPECT_TRUE(shm.enabled);
  EXPECT_EQ(shm.shm_name, "aquila_binance_market_data");
  EXPECT_EQ(shm.channel_name, "book_ticker_channel");
  EXPECT_TRUE(shm.create);
  EXPECT_TRUE(shm.remove_existing);
}

TEST(DataSessionConfigTest, RejectsRuntimeBookTickerShmCapacity) {
  const std::string toml_text = std::string{R"toml(
[instrument_catalog]
file = ")toml"} + SourcePath("config/instruments/usdt_futures.csv").string() +
                                R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "gate_data_session"
subscribe_symbols = ["BTC_USDT"]
settle = "usdt"
wire_format = "sbe"
sbe_schema_id = 1
feed = "book_ticker"

[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"
enable_tls = false

[data_session.websocket.execution_policy]
bind_cpu_id = 2

[data_shm_sink]
enabled = true
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
capacity = 65536
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::gate::ParseDataSessionConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_shm_sink.capacity is not supported"),
            std::string::npos);
}

TEST(DataSessionConfigTest, RejectsDataShmSinkExpectedCapacityKey) {
  const std::string toml_text = std::string{R"toml(
[instrument_catalog]
file = ")toml"} + SourcePath("config/instruments/usdt_futures.csv").string() +
                                R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "binance_data_session"
subscribe_symbols = ["BTC_USDT"]
market = "um_futures"
feed = "book_ticker"

[data_session.websocket.endpoint]
host = "fstream.binance.com"

[data_session.websocket.execution_policy]
bind_cpu_id = 3

[data_shm_sink]
enabled = true
shm_name = "aquila_binance_market_data"
channel_name = "book_ticker_channel"
expected_capacity = 65536
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::binance::ParseDataSessionConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(
      result.error.find("data_shm_sink.expected_capacity is not supported"),
      std::string::npos);
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
  EXPECT_EQ(config.connection.port, "443");
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
  EXPECT_TRUE(config.book_ticker_shm.enabled);
  EXPECT_EQ(config.book_ticker_shm.shm_name, "aquila_binance_market_data");
  EXPECT_EQ(config.book_ticker_shm.channel_name, "book_ticker_channel");
  EXPECT_TRUE(config.book_ticker_shm.create);
  EXPECT_FALSE(config.book_ticker_shm.remove_existing);

  struct DataSink {
    void OnBookTicker(const aquila::BookTicker&) noexcept {}
  } data_sink;

  using Session = aquila::binance::DataSession<
      DataSink, aquila::binance::DefaultTlsWebSocketPolicy,
      aquila::binance::SessionOnlyDiagnosticsPolicy>;
  Session session(config, data_sink);
  EXPECT_EQ(session.name(), "binance_data_session");
  EXPECT_EQ(session.connection().host, "fstream.binance.com");
  EXPECT_EQ(session.connection().port, "443");
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

TEST(DataSessionConfigTest, LoadsReadyFirst5BinanceDataSessionConfig) {
  const auto config_result =
      aquila::binance::LoadDataSessionConfigFile(SourcePath(
          "config/data_sessions/binance_data_session_first5_20260521.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const aquila::binance::DataSessionConfig& config = config_result.value;
  EXPECT_EQ(config.name, "binance_data_session_first5");
  ASSERT_EQ(config.exchange_symbols.size(), 5u);
  EXPECT_EQ(config.exchange_symbols[0], "PROVEUSDT");
  EXPECT_EQ(config.exchange_symbols[1], "RAVEUSDT");
  EXPECT_EQ(config.exchange_symbols[2], "ZECUSDT");
  EXPECT_EQ(config.exchange_symbols[3], "SIRENUSDT");
  EXPECT_EQ(config.exchange_symbols[4], "ETCUSDT");
  EXPECT_EQ(config.book_ticker_shm.shm_name,
            "aquila_binance_market_data_first5_20260521");
  EXPECT_TRUE(config.book_ticker_shm.remove_existing);
}

TEST(DataSessionConfigTest, LoadsReadyRequestedBinanceDataSessionConfig) {
  const auto config_result =
      aquila::binance::LoadDataSessionConfigFile(SourcePath(
          "config/data_sessions/binance_data_session_requested_20260521.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const aquila::binance::DataSessionConfig& config = config_result.value;
  EXPECT_EQ(config.name, "binance_data_session_requested");
  ASSERT_EQ(config.exchange_symbols.size(), 12u);
  EXPECT_EQ(config.exchange_symbols[0], "PROVEUSDT");
  EXPECT_EQ(config.exchange_symbols[1], "RAVEUSDT");
  EXPECT_EQ(config.exchange_symbols[2], "ZECUSDT");
  EXPECT_EQ(config.exchange_symbols[3], "SIRENUSDT");
  EXPECT_EQ(config.exchange_symbols[4], "ETCUSDT");
  EXPECT_EQ(config.exchange_symbols[5], "DASHUSDT");
  EXPECT_EQ(config.exchange_symbols[6], "RIVERUSDT");
  EXPECT_EQ(config.exchange_symbols[7], "SUIUSDT");
  EXPECT_EQ(config.exchange_symbols[8], "INJUSDT");
  EXPECT_EQ(config.exchange_symbols[9], "ENAUSDT");
  EXPECT_EQ(config.exchange_symbols[10], "BRETTUSDT");
  EXPECT_EQ(config.exchange_symbols[11], "ETHUSDT");
  EXPECT_EQ(config.symbol_ids[0], 4);
  EXPECT_EQ(config.symbol_ids[10], 14);
  EXPECT_EQ(config.symbol_ids[11], 1);
  EXPECT_EQ(config.book_ticker_shm.shm_name,
            "aquila_binance_market_data_requested_20260521");
  EXPECT_TRUE(config.book_ticker_shm.remove_existing);
}

TEST(DataSessionConfigTest, LoadsReadyLabUsdtBinanceDataSessionConfig) {
  const auto config_result = aquila::binance::LoadDataSessionConfigFile(
      SourcePath(
          "config/data_sessions/binance_data_session_lab_usdt_20260601.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const aquila::binance::DataSessionConfig& config = config_result.value;
  EXPECT_EQ(config.name, "binance_data_session_lab_usdt_20260601");
  EXPECT_EQ(config.connection.host, "fstream.binance.com");
  EXPECT_EQ(config.connection.port, "443");
  EXPECT_TRUE(config.connection.enable_tls);
  EXPECT_EQ(config.connection.target, "/public/ws/labusdt@bookTicker");
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 3);

  ASSERT_EQ(config.exchange_symbols.size(), 1u);
  EXPECT_EQ(config.exchange_symbols[0], "LABUSDT");
  ASSERT_EQ(config.symbol_ids.size(), 1u);
  EXPECT_EQ(config.symbol_ids[0], 15);
  EXPECT_TRUE(config.book_ticker_shm.enabled);
  EXPECT_EQ(config.book_ticker_shm.shm_name,
            "aquila_binance_market_data_lab_usdt_20260601");
  EXPECT_TRUE(config.book_ticker_shm.create);
  EXPECT_TRUE(config.book_ticker_shm.remove_existing);
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
