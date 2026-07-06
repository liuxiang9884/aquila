#include "core/config/data_reader_config.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_set.h>

#include "nova/utils/log.h"

namespace aquila::config {
namespace {

void MaybeLogError(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_ERROR("{}", message);
  }
}

[[nodiscard]] DataReaderConfigResult Failure(std::string error) {
  MaybeLogError(error);
  DataReaderConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] DataReaderConfigResult Success(DataReaderConfig config) {
  DataReaderConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

[[nodiscard]] bool ParseExchange(std::string_view text, Exchange* exchange) {
  if (text == "gate") {
    *exchange = Exchange::kGate;
    return true;
  }
  if (text == "binance") {
    *exchange = Exchange::kBinance;
    return true;
  }
  return false;
}

[[nodiscard]] bool ParseSourceType(std::string_view text,
                                   DataReaderSourceType* type) {
  if (text == "shm") {
    *type = DataReaderSourceType::kShm;
    return true;
  }
  if (text == "binary_file") {
    *type = DataReaderSourceType::kBinaryFile;
    return true;
  }
  return false;
}

[[nodiscard]] bool ParseFeed(std::string_view text, DataReaderFeed* feed) {
  if (text == "book_ticker") {
    *feed = DataReaderFeed::kBookTicker;
    return true;
  }
  if (text == "trade") {
    *feed = DataReaderFeed::kTrade;
    return true;
  }
  return false;
}

[[nodiscard]] bool ParseStartPosition(std::string_view text,
                                      DataReaderStartPosition* position) {
  if (text == "latest") {
    *position = DataReaderStartPosition::kLatest;
    return true;
  }
  if (text == "earliest_visible") {
    *position = DataReaderStartPosition::kEarliestVisible;
    return true;
  }
  return false;
}

[[nodiscard]] bool ParseReadMode(std::string_view text,
                                 DataReaderReadMode* read_mode) {
  if (text == "latest") {
    *read_mode = DataReaderReadMode::kLatest;
    return true;
  }
  if (text == "drain") {
    *read_mode = DataReaderReadMode::kDrain;
    return true;
  }
  return false;
}

class DataReaderConfigParser {
 public:
  explicit DataReaderConfigParser(const toml::table& node) : node_(node) {}
  DataReaderConfigParser(const toml::table& node,
                         std::filesystem::path config_file_path)
      : node_(node), config_file_path_(std::move(config_file_path)) {}

  [[nodiscard]] DataReaderConfigResult Parse() {
    ParseInstrumentCatalog();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseDataReader();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseExecutionPolicy();
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseSources();
    if (!ok_) {
      return Failure(std::move(error_));
    }

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

  [[nodiscard]] std::uint32_t UInt32Or(
      toml::node_view<const toml::node> value_node, std::uint32_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value <= 0) {
      Fail(name, " must be positive");
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
      toml::node_view<const toml::node> value_node,
      std::int32_t fallback) const {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    return static_cast<std::int32_t>(*value);
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
    instrument_catalog_.file =
        RequiredString(catalog["file"], "instrument_catalog.file");
    if (!ok_) {
      return;
    }
    instrument_catalog_.schema =
        RequiredString(catalog["schema"], "instrument_catalog.schema");
  }

  void ParseDataReader() {
    const toml::node_view<const toml::node> data_reader = node_["data_reader"];
    config_.name = RequiredString(data_reader["name"], "data_reader.name");
    if (!ok_) {
      return;
    }

    if (data_reader["max_events_per_source"]) {
      Fail("data_reader.max_events_per_source",
           " was removed; use data_reader.max_events_per_drain");
      return;
    }

    config_.max_events_per_drain = UInt32Or(data_reader["max_events_per_drain"],
                                            config_.max_events_per_drain,
                                            "data_reader.max_events_per_drain");
  }

  void ParseExecutionPolicy() {
    const toml::node_view<const toml::node> execution_policy =
        node_["data_reader"]["execution_policy"];
    config_.execution_policy.bind_cpu_id = Int32Or(
        execution_policy["bind_cpu_id"], config_.execution_policy.bind_cpu_id);
    config_.execution_policy.idle_policy = StringOr(
        execution_policy["idle_policy"], config_.execution_policy.idle_policy);
  }

  void ParseSources() {
    const toml::array* sources = node_["data_reader"]["sources"].as_array();
    if (sources == nullptr || sources->empty()) {
      Fail("data_reader.sources", " is required");
      return;
    }

    absl::flat_hash_set<std::string> source_names;
    config_.sources.reserve(sources->size());
    for (const toml::node& source_node : *sources) {
      const toml::table* source_table = source_node.as_table();
      if (source_table == nullptr) {
        Fail("data_reader.sources", " entries must be tables");
        return;
      }

      DataReaderSourceConfig source;
      source.name =
          RequiredString((*source_table)["name"], "data_reader.sources.name");
      if (!ok_) {
        return;
      }
      if (!source_names.insert(source.name).second) {
        Fail("data_reader.sources.name", " must be unique");
        return;
      }

      ParseSourceTypeField(*source_table, &source);
      if (!ok_) {
        return;
      }
      ParseExchangeField(*source_table, &source);
      if (!ok_) {
        return;
      }
      ParseFeedField(*source_table, &source);
      if (!ok_) {
        return;
      }
      ParseStartPositionField(*source_table, &source);
      if (!ok_) {
        return;
      }
      ParseReadModeField(*source_table, &source);
      if (!ok_) {
        return;
      }
      ParseFilesField(*source_table, &source);
      if (!ok_) {
        return;
      }

      source.shm_name = StringOr((*source_table)["shm_name"], source.shm_name);
      source.channel_name =
          StringOr((*source_table)["channel_name"], source.channel_name);
      source.required = BoolOr((*source_table)["required"], source.required);

      ValidateSource(source);
      if (!ok_) {
        return;
      }

      config_.sources.push_back(std::move(source));
    }
  }

  void ParseSourceTypeField(const toml::table& source_table,
                            DataReaderSourceConfig* source) {
    const std::string type_text =
        StringOr(source_table["type"], std::string{"shm"});
    if (!ParseSourceType(type_text, &source->type)) {
      Fail("data_reader.sources.type", " must be shm or binary_file");
    }
  }

  void ParseExchangeField(const toml::table& source_table,
                          DataReaderSourceConfig* source) {
    if (source->type == DataReaderSourceType::kBinaryFile &&
        !source_table["exchange"]) {
      return;
    }

    const std::string exchange_text = RequiredString(
        source_table["exchange"], "data_reader.sources.exchange");
    if (!ok_) {
      return;
    }
    if (!ParseExchange(exchange_text, &source->exchange)) {
      Fail("data_reader.sources.exchange", " must be gate or binance");
    }
  }

  void ParseFeedField(const toml::table& source_table,
                      DataReaderSourceConfig* source) {
    if (source->type == DataReaderSourceType::kBinaryFile &&
        !source_table["feed"]) {
      Fail("data_reader.sources.feed", " is required for binary_file sources");
      return;
    }
    const std::string feed_text =
        StringOr(source_table["feed"], std::string{"book_ticker"});
    if (!ParseFeed(feed_text, &source->feed)) {
      Fail("data_reader.sources.feed", " must be book_ticker or trade");
    }
  }

  void ParseStartPositionField(const toml::table& source_table,
                               DataReaderSourceConfig* source) {
    const std::string default_start_position =
        source->type == DataReaderSourceType::kBinaryFile
            ? std::string{"earliest_visible"}
            : std::string{"latest"};
    const std::string start_position_text =
        StringOr(source_table["start_position"], default_start_position);
    if (!ParseStartPosition(start_position_text, &source->start_position)) {
      Fail("data_reader.sources.start_position",
           " must be latest or earliest_visible");
    }
  }

  void ParseReadModeField(const toml::table& source_table,
                          DataReaderSourceConfig* source) {
    const std::string default_read_mode =
        source->type == DataReaderSourceType::kBinaryFile
            ? std::string{"drain"}
            : std::string{"latest"};
    const std::string read_mode_text =
        StringOr(source_table["read_mode"], default_read_mode);
    if (!ParseReadMode(read_mode_text, &source->read_mode)) {
      Fail("data_reader.sources.read_mode", " must be latest or drain");
    }
  }

  void ParseFilesField(const toml::table& source_table,
                       DataReaderSourceConfig* source) {
    const toml::array* files = source_table["files"].as_array();
    if (files == nullptr) {
      return;
    }
    source->files.reserve(files->size());
    for (const toml::node& file_node : *files) {
      const std::optional<std::string> file_path =
          file_node.value<std::string>();
      if (!file_path || file_path->empty()) {
        Fail("data_reader.sources.files", " entries must be non-empty strings");
        return;
      }
      source->files.emplace_back(*file_path);
    }
  }

  void ValidateSource(const DataReaderSourceConfig& source) {
    if (source.type == DataReaderSourceType::kShm) {
      if (source.shm_name.empty()) {
        Fail("data_reader.sources.shm_name", " is required");
        return;
      }
      if (source.channel_name.empty()) {
        Fail("data_reader.sources.channel_name", " is required");
        return;
      }
    }
    if (source.type == DataReaderSourceType::kBinaryFile) {
      if (source.files.empty()) {
        Fail("data_reader.sources.files", " is required");
        return;
      }
      if (source.start_position == DataReaderStartPosition::kLatest) {
        Fail("data_reader.sources.start_position",
             " must be earliest_visible for binary_file sources");
        return;
      }
      if (source.read_mode != DataReaderReadMode::kDrain) {
        Fail("data_reader.sources.read_mode",
             " must be drain for binary_file sources");
        return;
      }
    }
    switch (source.feed) {
      case DataReaderFeed::kBookTicker:
      case DataReaderFeed::kTrade:
        break;
      default:
        Fail("data_reader.sources.feed", " must be book_ticker or trade");
    }
  }

  [[nodiscard]] DataReaderConfigResult BuildConfig() {
    if (!ok_) {
      return Failure(std::move(error_));
    }
    if (config_.sources.empty()) {
      return Failure("data_reader.sources is required");
    }

    const InstrumentCatalogLoadResult catalog_result =
        LoadInstrumentCatalogFromCsv(ResolveInstrumentCatalogPath());
    if (!catalog_result.ok) {
      return Failure(catalog_result.error);
    }
    config_.instrument_catalog = std::move(catalog_result.value);
    return Success(std::move(config_));
  }

  [[nodiscard]] std::filesystem::path ResolveInstrumentCatalogPath() const {
    const std::filesystem::path catalog_path{instrument_catalog_.file};
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
  InstrumentCatalogConfig instrument_catalog_;
  DataReaderConfig config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

DataReaderConfigResult ParseDataReaderConfig(const toml::table& node) {
  return DataReaderConfigParser{node}.Parse();
}

DataReaderConfigResult ParseDataReaderConfig(
    const toml::table& node, const std::filesystem::path& config_file_path) {
  return DataReaderConfigParser{node, config_file_path}.Parse();
}

DataReaderConfigResult LoadDataReaderConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseDataReaderConfig(parsed, path);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load data reader config: "} +
                   exc.what());
  }
}

}  // namespace aquila::config
