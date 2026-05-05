#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_DATA_SESSION_CONFIG_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_DATA_SESSION_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "core/config/websocket_config.h"
#include "core/websocket/types.h"
#include "exchange/gate/market_data/client.h"

namespace aquila::gate {

struct GateFutureMarketDataSessionConfig {
  std::string name;
  std::vector<std::string> subscribe_symbols;
  std::string settle{"usdt"};
  std::string wire_format{"sbe"};
  std::uint32_t sbe_schema_id{1};
  std::string feed{"book_ticker"};
  config::WebSocketConfig websocket;
};

struct GateFutureMarketDataConfigFile {
  config::InstrumentCatalogConfig instrument_catalog;
  GateFutureMarketDataSessionConfig data_session;
};

struct GateFutureMarketDataConfigResult {
  GateFutureMarketDataConfigFile config;
  std::string error;
  bool ok{false};
};

struct GateFutureMarketDataSessionSettings {
  std::string name;
  websocket::ConnectionConfig connection;
  std::vector<std::string> exchange_symbols;
  std::vector<SymbolBinding> symbols;
};

struct GateFutureMarketDataSessionSettingsResult {
  GateFutureMarketDataSessionSettings settings;
  std::string error;
  bool ok{false};
};

[[nodiscard]] GateFutureMarketDataConfigResult
ParseGateFutureMarketDataConfig(const toml::table& node);

[[nodiscard]] GateFutureMarketDataConfigResult
LoadGateFutureMarketDataConfigFile(const std::filesystem::path& path);

[[nodiscard]] inline std::string BuildGateFutureMarketDataTarget(
    const GateFutureMarketDataSessionConfig& config) {
  return fmt::format("/v4/ws/{}/sbe?sbe_schema_id={}", config.settle,
                     config.sbe_schema_id);
}

[[nodiscard]] inline GateFutureMarketDataSessionSettingsResult
BuildGateFutureMarketDataSessionSettings(
    const GateFutureMarketDataConfigFile& config_file,
    const config::InstrumentCatalog& catalog) {
  GateFutureMarketDataSessionSettingsResult result;
  const GateFutureMarketDataSessionConfig& data_session =
      config_file.data_session;
  if (data_session.feed != "book_ticker" || data_session.wire_format != "sbe") {
    result.error = "Gate future market data supports only SBE book_ticker";
    return result;
  }

  const std::string target = BuildGateFutureMarketDataTarget(data_session);
  config::ConnectionConfigResult connection_result =
      config::ToConnectionConfig(data_session.websocket, target);
  if (!connection_result.ok) {
    result.error = std::move(connection_result.error);
    return result;
  }

  result.settings.name = data_session.name;
  result.settings.connection = std::move(connection_result.config);
  result.settings.exchange_symbols.reserve(
      data_session.subscribe_symbols.size());
  std::vector<std::int32_t> symbol_ids;
  symbol_ids.reserve(data_session.subscribe_symbols.size());

  for (const std::string& symbol : data_session.subscribe_symbols) {
    const config::InstrumentInfo* info = catalog.Find(Exchange::kGate, symbol);
    if (info == nullptr) {
      result.error = fmt::format("Gate instrument not found: {}", symbol);
      return result;
    }
    result.settings.exchange_symbols.push_back(info->exchange_symbol);
    symbol_ids.push_back(info->symbol_id);
  }

  result.settings.symbols.reserve(result.settings.exchange_symbols.size());
  for (std::size_t i = 0; i < result.settings.exchange_symbols.size(); ++i) {
    result.settings.symbols.push_back(SymbolBinding{
        .exchange_symbol = result.settings.exchange_symbols[i],
        .symbol_id = symbol_ids[i],
    });
  }

  result.ok = true;
  return result;
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_DATA_SESSION_CONFIG_H_
