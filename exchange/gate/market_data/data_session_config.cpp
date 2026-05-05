#include "exchange/gate/market_data/data_session_config.h"

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "nova/utils/log.h"

namespace aquila::gate {
namespace {

void MaybeLogError(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_ERROR("{}", message);
  }
}

[[nodiscard]] FuturesMarketDataConfigResult Failure(std::string error) {
  MaybeLogError(error);
  FuturesMarketDataConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] FuturesMarketDataConfigResult Success(
    FuturesMarketDataConfigFile config) {
  FuturesMarketDataConfigResult result;
  result.config = std::move(config);
  result.ok = true;
  return result;
}

class FuturesMarketDataConfigParser {
 public:
  explicit FuturesMarketDataConfigParser(const toml::table& node)
      : node_(node) {}

  [[nodiscard]] FuturesMarketDataConfigResult Parse() {
    ParseInstrumentCatalog();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseDataSession();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    const config::WebSocketConfigResult websocket_result =
        config::ParseWebSocketConfig(node_["data_session"]["websocket"]);
    if (!websocket_result.ok) {
      return Failure(websocket_result.error);
    }
    config_.data_session.websocket = std::move(websocket_result.config);
    return Success(std::move(config_));
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

  void Fail(std::string_view name, std::string_view message) {
    ok_ = false;
    error_.assign(name);
    error_.append(message);
  }

  const toml::table& node_;
  FuturesMarketDataConfigFile config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

FuturesMarketDataConfigResult ParseFuturesMarketDataConfig(
    const toml::table& node) {
  return FuturesMarketDataConfigParser{node}.Parse();
}

FuturesMarketDataConfigResult LoadFuturesMarketDataConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseFuturesMarketDataConfig(parsed);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load Gate market data config: "} +
                   exc.what());
  }
}

}  // namespace aquila::gate
