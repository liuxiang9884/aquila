#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_DATA_SESSION_CONFIG_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_DATA_SESSION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/config/instrument_catalog.h"
#include "core/config/websocket_config.h"

namespace aquila::gate {

struct DataSessionConfig {
  std::string name;
  std::vector<std::string> subscribe_symbols;
  std::string settle{"usdt"};
  std::string wire_format{"sbe"};
  std::uint32_t sbe_schema_id{1};
  std::string feed{"book_ticker"};
  config::WebSocketConfig websocket;
};

struct DataSessionConfigFile {
  config::InstrumentCatalogConfig instrument_catalog;
  DataSessionConfig data_session;
};

struct DataSessionConfigResult {
  DataSessionConfigFile config;
  std::string error;
  bool ok{false};
};

[[nodiscard]] DataSessionConfigResult ParseDataSessionConfig(
    const toml::table& node);

[[nodiscard]] DataSessionConfigResult LoadDataSessionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_DATA_SESSION_CONFIG_H_
