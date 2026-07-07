#include "exchange/gate/market_data/data_session_config.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common/data_session_diagnostic_level.h"
#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "core/config/websocket_config.h"
#include "core/market_data/data_session_diagnostics.h"
#include "core/market_data/data_shm_config.h"
#include "nova/utils/log.h"

namespace aquila::gate {
namespace {

struct RawDataSessionConfig {
  std::string name;
  std::vector<std::string> subscribe_symbols;
  std::string settle{"usdt"};
  std::string wire_format{"sbe"};
  std::uint32_t sbe_schema_id{1};
  DataSessionFeeds feeds;
  config::WebSocketConfig websocket;
};

struct RawConfigFile {
  config::InstrumentCatalogConfig instrument_catalog;
  RawDataSessionConfig data_session;
  ::aquila::market_data::DataShmConfig data_shm;
  ::aquila::market_data::BookTickerShmConfig book_ticker_shm;
  ::aquila::market_data::TradeShmConfig trade_shm;
  ::aquila::market_data::DataSessionDiagnosticsConfig diagnostics;
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

    ParseDataShmSink();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseDiagnostics();
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
      std::uint32_t fallback, std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value < 0) {
      Fail(name, " must be non-negative");
      return fallback;
    }
    if (*value >
        static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
      Fail(name, " exceeds uint32 max");
      return fallback;
    }
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] std::int32_t Int32Or(
      toml::node_view<const toml::node> value_node, std::int32_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value < 0 || *value > std::numeric_limits<std::int32_t>::max()) {
      Fail(name, " must be a non-negative int32");
      return fallback;
    }
    return static_cast<std::int32_t>(*value);
  }

  [[nodiscard]] std::int64_t NonNegativeInt64Or(
      toml::node_view<const toml::node> value_node, std::int64_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value < 0) {
      Fail(name, " must be non-negative");
      return fallback;
    }
    return *value;
  }

  [[nodiscard]] std::uint32_t NonNegativeUint32Or(
      toml::node_view<const toml::node> value_node, std::uint32_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value < 0 || *value > std::numeric_limits<std::uint32_t>::max()) {
      Fail(name, " must be a non-negative uint32");
      return fallback;
    }
    return static_cast<std::uint32_t>(*value);
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
    config_.data_session.settle =
        StringOr(data_session["settle"], config_.data_session.settle);
    config_.data_session.wire_format =
        StringOr(data_session["wire_format"], config_.data_session.wire_format);
    config_.data_session.sbe_schema_id = UInt32Or(
        data_session["sbe_schema_id"], config_.data_session.sbe_schema_id,
        "data_session.sbe_schema_id");
    if (!ok_) {
      return;
    }
    ParseFeeds(data_session);
  }

  void ParseFeeds(toml::node_view<const toml::node> data_session) {
    const bool has_feed = static_cast<bool>(data_session["feed"]);
    const bool has_feeds = static_cast<bool>(data_session["feeds"]);
    if (has_feed && has_feeds) {
      Fail("data_session.feed and data_session.feeds",
           " cannot both be configured");
      return;
    }

    if (has_feeds) {
      config_.data_session.feeds =
          DataSessionFeeds{.book_ticker = false, .trade = false};
      const toml::array* feeds = data_session["feeds"].as_array();
      if (feeds == nullptr || feeds->empty()) {
        Fail("data_session.feeds", " must contain at least one feed");
        return;
      }
      for (const toml::node& feed_node : *feeds) {
        const std::optional<std::string> feed = feed_node.value<std::string>();
        if (!feed || feed->empty()) {
          Fail("data_session.feeds", " must contain strings");
          return;
        }
        SetFeed(*feed);
        if (!ok_) {
          return;
        }
      }
      return;
    }

    if (has_feed) {
      config_.data_session.feeds =
          DataSessionFeeds{.book_ticker = false, .trade = false};
      const std::string feed =
          RequiredString(data_session["feed"], "data_session.feed");
      if (!ok_) {
        return;
      }
      SetFeed(feed);
    }
  }

  void SetFeed(std::string_view feed) {
    if (feed == "book_ticker") {
      if (config_.data_session.feeds.book_ticker) {
        Fail("data_session.feeds", " contains duplicate feed book_ticker");
        return;
      }
      config_.data_session.feeds.book_ticker = true;
      return;
    }
    if (feed == "trade") {
      if (config_.data_session.feeds.trade) {
        Fail("data_session.feeds", " contains duplicate feed trade");
        return;
      }
      config_.data_session.feeds.trade = true;
      return;
    }
    std::string message{" unknown Gate data_session feed: "};
    message.append(feed);
    Fail("data_session.feeds", message);
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

    config_.data_shm.enabled = BoolOr(shm["enabled"], config_.data_shm.enabled);
    config_.data_shm.shm_name =
        StringOr(shm["shm_name"], config_.data_shm.shm_name);
    const std::string legacy_book_ticker_channel = StringOr(
        shm["channel_name"], config_.data_shm.book_ticker_channel_name);
    config_.data_shm.book_ticker_channel_name =
        StringOr(shm["book_ticker_channel_name"], legacy_book_ticker_channel);
    config_.data_shm.trade_channel_name = StringOr(
        shm["trade_channel_name"], config_.data_shm.trade_channel_name);
    config_.data_shm.create = BoolOr(shm["create"], config_.data_shm.create);
    config_.data_shm.remove_existing =
        BoolOr(shm["remove_existing"], config_.data_shm.remove_existing);
    config_.data_shm.book_ticker_enabled =
        config_.data_session.feeds.book_ticker;
    config_.data_shm.trade_enabled = config_.data_session.feeds.trade;

    config_.book_ticker_shm = config_.data_shm.BookTickerConfig();
    config_.trade_shm = config_.data_shm.TradeConfig();

    if (!config_.data_shm.create && config_.data_shm.remove_existing) {
      Fail("data_shm_sink.remove_existing", " requires create=true");
      return;
    }
    if (!config_.data_shm.enabled) {
      return;
    }
    if (config_.data_shm.shm_name.empty()) {
      Fail("data_shm_sink.shm_name", " is required");
      return;
    }
    if (config_.data_session.feeds.book_ticker &&
        config_.data_shm.book_ticker_channel_name.empty()) {
      Fail("data_shm_sink.book_ticker_channel_name", " is required");
      return;
    }
    if (config_.data_session.feeds.trade &&
        config_.data_shm.trade_channel_name.empty()) {
      Fail("data_shm_sink.trade_channel_name", " is required");
      return;
    }
    if (config_.data_session.feeds.book_ticker &&
        config_.data_session.feeds.trade &&
        config_.data_shm.book_ticker_channel_name ==
            config_.data_shm.trade_channel_name) {
      Fail("data_shm_sink.channel_name",
           " book_ticker and trade channels must be distinct");
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

  void ParseDiagnostics() {
    const toml::node_view<const toml::node> diagnostics =
        node_["data_session"]["diagnostics"];
    const toml::node_view<const toml::node> latency =
        diagnostics["latency_outlier"];
    auto& latency_config = config_.diagnostics.latency_outlier;
    latency_config.enabled = BoolOr(latency["enabled"], latency_config.enabled);
    latency_config.source_id =
        Int32Or(latency["source_id"], latency_config.source_id,
                "data_session.diagnostics.latency_outlier.source_id");
    if (!ok_) {
      return;
    }
    latency_config.threshold_ns =
        NonNegativeInt64Or(latency["threshold_ns"], latency_config.threshold_ns,
                           "data_session.diagnostics.latency_outlier."
                           "threshold_ns");
    if (!ok_) {
      return;
    }
    latency_config.max_logs_per_second = NonNegativeUint32Or(
        latency["max_logs_per_second"], latency_config.max_logs_per_second,
        "data_session.diagnostics.latency_outlier."
        "max_logs_per_second");
    if (!ok_) {
      return;
    }

    ParseSocketTimestampingConfig(diagnostics["timestamping"],
                                  &config_.diagnostics.socket_timestamping,
                                  "data_session.diagnostics.timestamping");
    if (!ok_) {
      return;
    }
    ValidateDiagnostics();
  }

  void ParseSocketTimestampingConfig(
      toml::node_view<const toml::node> node,
      websocket::SocketTimestampingConfig* timestamping,
      std::string_view name) {
    timestamping->enabled = BoolOr(node["enabled"], timestamping->enabled);
    timestamping->tx_sched = BoolOr(node["tx_sched"], timestamping->tx_sched);
    timestamping->tx_software =
        BoolOr(node["tx_software"], timestamping->tx_software);
    timestamping->tx_ack = BoolOr(node["tx_ack"], timestamping->tx_ack);
    timestamping->rx_software =
        BoolOr(node["rx_software"], timestamping->rx_software);
    timestamping->hardware = BoolOr(node["hardware"], timestamping->hardware);
    timestamping->max_errqueue_events_per_drain = NonNegativeUint32Or(
        node["max_errqueue_events_per_drain"],
        timestamping->max_errqueue_events_per_drain,
        std::string{name} + ".max_errqueue_events_per_drain");
    if (!ok_) {
      return;
    }
    timestamping->max_active_probes = NonNegativeUint32Or(
        node["max_active_probes"], timestamping->max_active_probes,
        std::string{name} + ".max_active_probes");
  }

  void ValidateDiagnostics() {
    if (config_.diagnostics.latency_outlier.enabled &&
        !core::DataSessionDiagnosticLevelSupports(1)) {
      Fail("data_session.diagnostics.latency_outlier.enabled",
           " requires AQUILA_DATA_SESSION_DIAG_LEVEL >= 1");
      return;
    }
    if (config_.diagnostics.socket_timestamping.enabled &&
        !core::DataSessionDiagnosticLevelSupports(
            core::RequiredDataSessionDiagnosticLevelForSocketTimestamping())) {
      Fail("data_session.diagnostics.timestamping.enabled",
           " requires AQUILA_DATA_SESSION_DIAG_LEVEL >= 4");
    }
  }

  [[nodiscard]] DataSessionConfigResult BuildConfig() {
    if (config_.data_session.wire_format != "sbe") {
      return Failure("Gate data session supports only SBE feeds");
    }
    if (!config_.data_session.feeds.book_ticker &&
        !config_.data_session.feeds.trade) {
      return Failure("Gate data session requires at least one feed");
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
    data_session_config.connection.socket_timestamping =
        config_.diagnostics.socket_timestamping;
    data_session_config.feeds = config_.data_session.feeds;
    data_session_config.data_shm = std::move(config_.data_shm);
    data_session_config.book_ticker_shm = std::move(config_.book_ticker_shm);
    data_session_config.trade_shm = std::move(config_.trade_shm);
    data_session_config.diagnostics = config_.diagnostics;
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
