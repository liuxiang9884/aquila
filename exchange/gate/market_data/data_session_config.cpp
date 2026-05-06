#include "exchange/gate/market_data/data_session_config.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "core/config/websocket_config.h"
#include "nova/utils/log.h"

namespace aquila::gate {
namespace {

struct RawDataSessionConfig {
  std::string name;
  std::vector<std::string> subscribe_symbols;
  std::string settle{"usdt"};
  std::string wire_format{"sbe"};
  std::uint32_t sbe_schema_id{1};
  std::string feed{"book_ticker"};
  config::WebSocketConfig websocket;
};

struct RawConfigFile {
  config::InstrumentCatalogConfig instrument_catalog;
  RawDataSessionConfig data_session;
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

[[nodiscard]] std::string BuildDataSessionTarget(
    const RawDataSessionConfig& config) {
  std::string target{"/v4/ws/"};
  target.append(config.settle);
  target.append("/sbe?sbe_schema_id=");
  std::array<char, 16> schema_id_buffer{};
  const auto [end, error] = std::to_chars(
      schema_id_buffer.data(),
      schema_id_buffer.data() + schema_id_buffer.size(), config.sbe_schema_id);
  (void)error;
  target.append(schema_id_buffer.data(), end);
  return target;
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

  [[nodiscard]] std::uint32_t UInt32Or(
      toml::node_view<const toml::node> value_node,
      std::uint32_t fallback) const {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    return static_cast<std::uint32_t>(*value);
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
    config_.data_session.settle =
        StringOr(data_session["settle"], config_.data_session.settle);
    config_.data_session.wire_format =
        StringOr(data_session["wire_format"], config_.data_session.wire_format);
    config_.data_session.sbe_schema_id = UInt32Or(
        data_session["sbe_schema_id"], config_.data_session.sbe_schema_id);
    config_.data_session.feed =
        StringOr(data_session["feed"], config_.data_session.feed);
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
    if (config_.data_session.feed != "book_ticker" ||
        config_.data_session.wire_format != "sbe") {
      return Failure("Gate data session supports only SBE book_ticker");
    }

    config::ConnectionConfigResult connection_result =
        config::ToConnectionConfig(
            config_.data_session.websocket,
            BuildDataSessionTarget(config_.data_session));
    if (!connection_result.ok) {
      return Failure(connection_result.error);
    }

    const config::InstrumentCatalogLoadResult catalog_result =
        config::LoadInstrumentCatalogFromCsv(ResolveInstrumentCatalogPath());
    if (!catalog_result.ok) {
      return Failure(catalog_result.error);
    }

    DataSessionConfig data_session_config;
    data_session_config.name = std::move(config_.data_session.name);
    data_session_config.connection = std::move(connection_result.value);
    data_session_config.exchange_symbols.reserve(
        config_.data_session.subscribe_symbols.size());
    data_session_config.symbol_ids.reserve(
        config_.data_session.subscribe_symbols.size());

    for (const std::string& symbol : config_.data_session.subscribe_symbols) {
      const config::InstrumentInfo* info =
          catalog_result.value.Find(Exchange::kGate, symbol);
      if (info == nullptr) {
        std::string error{"Gate instrument not found: "};
        error.append(symbol);
        return Failure(std::move(error));
      }
      data_session_config.exchange_symbols.push_back(info->exchange_symbol);
      data_session_config.symbol_ids.push_back(info->symbol_id);
    }

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
    return Failure(std::string{"failed to load Gate market data config: "} +
                   exc.what());
  }
}

}  // namespace aquila::gate
