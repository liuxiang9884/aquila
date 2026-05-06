#include "exchange/binance/market_data/data_session_config.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "core/config/websocket_config.h"
#include "core/market_data/data_shm_config.h"
#include "exchange/binance/market_data/stream.h"
#include "nova/utils/log.h"

namespace aquila::binance {
namespace {

struct RawDataSessionConfig {
  std::string name;
  std::vector<std::string> subscribe_symbols;
  std::string market{"um_futures"};
  std::string feed{"book_ticker"};
  config::WebSocketConfig websocket;
};

struct RawConfigFile {
  config::InstrumentCatalogConfig instrument_catalog;
  RawDataSessionConfig data_session;
  ::aquila::market_data::BookTickerShmConfig book_ticker_shm;
};

void MaybeLogError(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_ERROR("{}", message);
  }
}

[[nodiscard]] DataSessionConfigResult Failure(std::string error) {
  MaybeLogError(error);
  DataSessionConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] DataSessionConfigResult Success(DataSessionConfig config) {
  DataSessionConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

class DataSessionConfigParser {
 public:
  explicit DataSessionConfigParser(const toml::table& node) : node_(node) {}
  DataSessionConfigParser(const toml::table& node,
                          std::filesystem::path config_file_path)
      : node_(node), config_file_path_(std::move(config_file_path)) {}

  [[nodiscard]] DataSessionConfigResult Parse() {
    ParseInstrumentCatalog();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseDataSession();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseDataShmSink();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    config::WebSocketConfigResult websocket_result =
        config::ParseWebSocketConfig(node_["data_session"]["websocket"]);
    if (!websocket_result.ok) {
      return Failure(websocket_result.error);
    }
    config_.data_session.websocket = std::move(websocket_result.value);
    return BuildConfig();
  }

 private:
  [[nodiscard]] std::string StringOr(
      toml::node_view<const toml::node> value_node,
      const std::string& fallback) const {
    const std::optional<std::string> value = value_node.value<std::string>();
    return value.value_or(fallback);
  }

  [[nodiscard]] bool BoolOr(toml::node_view<const toml::node> value_node,
                            bool fallback) const {
    const std::optional<bool> value = value_node.value<bool>();
    return value.value_or(fallback);
  }

  [[nodiscard]] std::string RequiredString(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::string> value = value_node.value<std::string>();
    if (!value || value->empty()) {
      Fail(name, " is required");
      return {};
    }
    return *value;
  }

  void ParseInstrumentCatalog() {
    const toml::node_view<const toml::node> catalog =
        node_["instrument_catalog"];
    config_.instrument_catalog.file =
        RequiredString(catalog["file"], "instrument_catalog.file");
    if (!ok_) {
      return;
    }
    config_.instrument_catalog.schema =
        RequiredString(catalog["schema"], "instrument_catalog.schema");
  }

  void ParseDataSession() {
    const toml::node_view<const toml::node> data_session =
        node_["data_session"];
    config_.data_session.name =
        RequiredString(data_session["name"], "data_session.name");
    if (!ok_) {
      return;
    }
    ParseSubscribeSymbols(data_session["subscribe_symbols"]);
    if (!ok_) {
      return;
    }
    config_.data_session.market =
        StringOr(data_session["market"], config_.data_session.market);
    config_.data_session.feed =
        StringOr(data_session["feed"], config_.data_session.feed);
  }

  void ParseDataShmSink() {
    const toml::node_view<const toml::node> shm = node_["data_shm_sink"];
    if (!shm) {
      return;
    }
    if (shm["capacity"]) {
      Fail("data_shm_sink.capacity",
           " is not supported; capacity is fixed in code");
      return;
    }
    if (shm["expected_capacity"]) {
      Fail("data_shm_sink.expected_capacity",
           " is not supported; capacity is fixed in code");
      return;
    }

    config_.book_ticker_shm.enabled =
        BoolOr(shm["enabled"], config_.book_ticker_shm.enabled);
    config_.book_ticker_shm.shm_name =
        StringOr(shm["shm_name"], config_.book_ticker_shm.shm_name);
    config_.book_ticker_shm.channel_name =
        StringOr(shm["channel_name"], config_.book_ticker_shm.channel_name);
    config_.book_ticker_shm.create =
        BoolOr(shm["create"], config_.book_ticker_shm.create);
    config_.book_ticker_shm.remove_existing =
        BoolOr(shm["remove_existing"], config_.book_ticker_shm.remove_existing);

    if (!config_.book_ticker_shm.create &&
        config_.book_ticker_shm.remove_existing) {
      Fail("data_shm_sink.remove_existing", " requires create=true");
      return;
    }
    if (!config_.book_ticker_shm.enabled) {
      return;
    }
    if (config_.book_ticker_shm.shm_name.empty()) {
      Fail("data_shm_sink.shm_name", " is required");
      return;
    }
    if (config_.book_ticker_shm.channel_name.empty()) {
      Fail("data_shm_sink.channel_name", " is required");
    }
  }

  void ParseSubscribeSymbols(toml::node_view<const toml::node> value_node) {
    const toml::array* symbols = value_node.as_array();
    if (symbols == nullptr || symbols->empty()) {
      Fail("data_session.subscribe_symbols", " is required");
      return;
    }

    config_.data_session.subscribe_symbols.reserve(symbols->size());
    for (const toml::node& symbol_node : *symbols) {
      const std::optional<std::string> symbol =
          symbol_node.value<std::string>();
      if (!symbol || symbol->empty()) {
        Fail("data_session.subscribe_symbols", " must contain strings");
        return;
      }
      config_.data_session.subscribe_symbols.push_back(*symbol);
    }
  }

  [[nodiscard]] DataSessionConfigResult BuildConfig() {
    if (config_.data_session.market != "um_futures" ||
        config_.data_session.feed != "book_ticker") {
      return Failure(
          "Binance data session supports only UM futures book_ticker");
    }

    const config::InstrumentCatalogLoadResult catalog_result =
        config::LoadInstrumentCatalogFromCsv(ResolveInstrumentCatalogPath());
    if (!catalog_result.ok) {
      return Failure(catalog_result.error);
    }

    DataSessionConfig data_session_config;
    data_session_config.name = std::move(config_.data_session.name);
    data_session_config.book_ticker_shm = std::move(config_.book_ticker_shm);
    data_session_config.exchange_symbols.reserve(
        config_.data_session.subscribe_symbols.size());
    data_session_config.symbol_ids.reserve(
        config_.data_session.subscribe_symbols.size());

    for (const std::string& symbol : config_.data_session.subscribe_symbols) {
      const config::InstrumentInfo* info =
          catalog_result.value.Find(Exchange::kBinance, symbol);
      if (info == nullptr) {
        std::string error{"Binance instrument not found: "};
        error.append(symbol);
        return Failure(std::move(error));
      }
      data_session_config.exchange_symbols.push_back(info->exchange_symbol);
      data_session_config.symbol_ids.push_back(info->symbol_id);
    }

    std::vector<std::string_view> stream_symbols;
    stream_symbols.reserve(data_session_config.exchange_symbols.size());
    for (const std::string& symbol : data_session_config.exchange_symbols) {
      stream_symbols.push_back(symbol);
    }
    config::ConnectionConfigResult connection_result =
        config::ToConnectionConfig(
            config_.data_session.websocket,
            BuildFuturesBookTickerStreamTarget(
                std::span<const std::string_view>(stream_symbols.data(),
                                                  stream_symbols.size())));
    if (!connection_result.ok) {
      return Failure(connection_result.error);
    }
    data_session_config.connection = std::move(connection_result.value);
    return Success(std::move(data_session_config));
  }

  [[nodiscard]] std::filesystem::path ResolveInstrumentCatalogPath() const {
    const std::filesystem::path catalog_path{config_.instrument_catalog.file};
    if (catalog_path.is_absolute() || config_file_path_.empty()) {
      return catalog_path;
    }

    std::filesystem::path base =
        std::filesystem::absolute(config_file_path_).parent_path();
    while (!base.empty()) {
      const std::filesystem::path candidate = base / catalog_path;
      if (std::filesystem::exists(candidate)) {
        return candidate;
      }
      if (base == base.root_path()) {
        break;
      }
      base = base.parent_path();
    }
    return catalog_path;
  }

  void Fail(std::string_view name, std::string_view message) {
    ok_ = false;
    error_.assign(name);
    error_.append(message);
  }

  const toml::table& node_;
  std::filesystem::path config_file_path_;
  RawConfigFile config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

DataSessionConfigResult ParseDataSessionConfig(const toml::table& node) {
  return DataSessionConfigParser{node}.Parse();
}

DataSessionConfigResult ParseDataSessionConfig(
    const toml::table& node, const std::filesystem::path& config_file_path) {
  return DataSessionConfigParser{node, config_file_path}.Parse();
}

DataSessionConfigResult LoadDataSessionConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseDataSessionConfig(parsed, path);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load Binance market data config: "} +
                   exc.what());
  }
}

}  // namespace aquila::binance
