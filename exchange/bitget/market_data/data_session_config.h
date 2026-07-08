#ifndef AQUILA_EXCHANGE_BITGET_MARKET_DATA_DATA_SESSION_CONFIG_H_
#define AQUILA_EXCHANGE_BITGET_MARKET_DATA_DATA_SESSION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/market_data/data_session_diagnostics.h"
#include "core/market_data/data_shm_config.h"
#include "core/websocket/types.h"

namespace aquila::bitget {

struct DataSessionFeeds {
  bool book_ticker{true};
  bool trade{false};
};

struct DataSessionConfig {
  std::string name;
  std::string inst_type{"usdt-futures"};
  websocket::ConnectionConfig connection;
  std::vector<std::string> exchange_symbols;
  std::vector<std::int32_t> symbol_ids;
  DataSessionFeeds feeds;
  ::aquila::market_data::DataShmConfig data_shm;
  ::aquila::market_data::BookTickerShmConfig book_ticker_shm;
  ::aquila::market_data::DataSessionDiagnosticsConfig diagnostics;
};

using DataSessionConfigResult = Result<DataSessionConfig>;

[[nodiscard]] DataSessionConfigResult ParseDataSessionConfig(
    const toml::table& node);

[[nodiscard]] DataSessionConfigResult ParseDataSessionConfig(
    const toml::table& node, const std::filesystem::path& config_file_path);

[[nodiscard]] DataSessionConfigResult LoadDataSessionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_MARKET_DATA_DATA_SESSION_CONFIG_H_
