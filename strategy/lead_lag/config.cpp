#include "strategy/lead_lag/config.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace aquila::strategy::leadlag {
namespace {

[[nodiscard]] ConfigResult Failure(std::string error) {
  ConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] ConfigResult Success(Config config) {
  ConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

[[nodiscard]] bool ParseExchange(std::string_view text, Exchange* exchange) {
  if (text == "binance") {
    *exchange = Exchange::kBinance;
    return true;
  }
  if (text == "gate") {
    *exchange = Exchange::kGate;
    return true;
  }
  if (text == "okx") {
    *exchange = Exchange::kOkx;
    return true;
  }
  if (text == "bybit") {
    *exchange = Exchange::kBybit;
    return true;
  }
  if (text == "bitget") {
    *exchange = Exchange::kBitget;
    return true;
  }
  if (text == "coinbase") {
    *exchange = Exchange::kCoinbase;
    return true;
  }
  return false;
}

class Parser {
 public:
  Parser(const toml::table& node, const config::InstrumentCatalog& catalog)
      : node_(node), catalog_(catalog) {}

  [[nodiscard]] ConfigResult Parse() {
    const toml::table* lead_lag = node_["lead_lag"].as_table();
    if (lead_lag == nullptr) {
      return Failure("lead_lag section is required");
    }

    config_.name = RequiredString(*lead_lag, "name", "lead_lag.name");
    if (!ok_) {
      return Failure(std::move(error_));
    }
    if (config_.name != "lead_lag") {
      return Failure("lead_lag.name must be lead_lag");
    }

    config_.version = RequiredString(*lead_lag, "version", "lead_lag.version");
    if (!ok_) {
      return Failure(std::move(error_));
    }
    if (config_.version != "1.0") {
      return Failure("lead_lag.version must be 1.0");
    }

    if (const toml::table* risk = (*lead_lag)["risk"].as_table();
        risk != nullptr) {
      config_.risk = ParseRisk(*risk);
      if (!ok_) {
        return Failure(std::move(error_));
      }
    }

    const toml::array* pairs = (*lead_lag)["pairs"].as_array();
    if (pairs == nullptr || pairs->empty()) {
      return Failure("lead_lag.pairs is required");
    }

    config_.pairs.reserve(pairs->size());
    std::vector<std::int32_t> symbol_ids;
    symbol_ids.reserve(pairs->size());
    for (std::size_t index = 0; index < pairs->size(); ++index) {
      const toml::table* pair_table = (*pairs)[index].as_table();
      if (pair_table == nullptr) {
        return Failure(PairName(index, " must be a table"));
      }

      PairConfig pair = ParsePair(*pair_table, index);
      if (!ok_) {
        return Failure(std::move(error_));
      }
      for (const std::int32_t symbol_id : symbol_ids) {
        if (symbol_id == pair.symbol_id) {
          return Failure(PairName(index,
                                  ".symbol_id duplicates an earlier "
                                  "lead_lag pair"));
        }
      }
      symbol_ids.push_back(pair.symbol_id);
      config_.pairs.push_back(std::move(pair));
    }

    return Success(std::move(config_));
  }

 private:
  [[nodiscard]] PairConfig ParsePair(const toml::table& table,
                                     std::size_t index) {
    PairConfig pair;
    const std::string prefix = PairName(index, "");

    pair.symbol = RequiredString(table, "symbol", prefix + ".symbol");
    pair.symbol_id = RequiredInt32(table, "symbol_id", prefix + ".symbol_id");
    pair.lead_exchange =
        RequiredExchange(table, "lead_exchange", prefix + ".lead_exchange");
    pair.lag_exchange =
        RequiredExchange(table, "lag_exchange", prefix + ".lag_exchange");
    pair.lag_taker_fee =
        RequiredDouble(table, "lag_taker_fee", prefix + ".lag_taker_fee");
    if (!ok_) {
      return pair;
    }
    if (pair.symbol_id < 0) {
      Fail(prefix + ".symbol_id", " must be non-negative");
      return pair;
    }
    if (pair.lead_exchange == pair.lag_exchange) {
      Fail(prefix, ".lead_exchange and lag_exchange must differ");
      return pair;
    }
    if (pair.lag_taker_fee < 0.0) {
      Fail(prefix + ".lag_taker_fee", " must be non-negative");
      return pair;
    }

    const toml::table* trigger = table["trigger"].as_table();
    if (trigger == nullptr) {
      Fail(prefix + ".trigger", " is required");
      return pair;
    }
    pair.trigger = ParseTrigger(*trigger, prefix + ".trigger");
    if (!ok_) {
      return pair;
    }

    const toml::table* execute = table["execute"].as_table();
    if (execute == nullptr) {
      Fail(prefix + ".execute", " is required");
      return pair;
    }
    pair.execute = ParseExecute(*execute, prefix + ".execute");
    if (!ok_) {
      return pair;
    }

    const toml::table* bbo_record = table["bbo_record"].as_table();
    if (bbo_record == nullptr) {
      Fail(prefix + ".bbo_record", " is required");
      return pair;
    }
    pair.bbo_record = ParseBboRecord(*bbo_record, prefix + ".bbo_record");
    if (!ok_) {
      return pair;
    }
    if (pair.bbo_record.stats_window_ns < pair.bbo_record.window_ns) {
      Fail(prefix + ".bbo_record.stats_window",
           " must be greater than or equal to bbo_record.window");
      return pair;
    }

    if (const toml::table* capacity = table["capacity"].as_table();
        capacity != nullptr) {
      pair.capacity = ParseCapacity(*capacity, prefix + ".capacity");
      if (!ok_) {
        return pair;
      }
    }

    if (!LoadInstrumentMetadata(&pair, index)) {
      return pair;
    }
    return pair;
  }

  [[nodiscard]] TriggerConfig ParseTrigger(const toml::table& table,
                                           const std::string& prefix) {
    TriggerConfig trigger;
    trigger.lead = RequiredDouble(table, "lead", prefix + ".lead");
    trigger.close = RequiredDouble(table, "close", prefix + ".close");
    trigger.lag_part = RequiredDouble(table, "lag_part", prefix + ".lag_part");
    trigger.target_profit_rate = RequiredDouble(table, "target_profit_rate",
                                                prefix + ".target_profit_rate");
    trigger.drift_limit =
        RequiredDouble(table, "drift_limit", prefix + ".drift_limit");
    trigger.drift_period_ns =
        RequiredDurationNs(table, "drift_period", prefix + ".drift_period");
    trigger.drift_min_samples = RequiredUInt32(table, "drift_min_samples",
                                               prefix + ".drift_min_samples");
    trigger.drift_warmup_ns =
        RequiredDurationNs(table, "drift_warmup", prefix + ".drift_warmup");
    if (!ok_) {
      return trigger;
    }
    if (trigger.drift_min_samples == 0) {
      Fail(prefix + ".drift_min_samples", " must be positive");
      return trigger;
    }

    const toml::table* quantile = table["quantile"].as_table();
    if (quantile == nullptr) {
      Fail(prefix + ".quantile", " is required");
      return trigger;
    }
    trigger.quantile = ParseQuantile(*quantile, prefix + ".quantile");
    return trigger;
  }

  [[nodiscard]] QuantileConfig ParseQuantile(const toml::table& table,
                                             const std::string& prefix) {
    QuantileConfig quantile;
    quantile.move = RequiredDouble(table, "move", prefix + ".move");
    quantile.up_min = RequiredDouble(table, "up_min", prefix + ".up_min");
    quantile.up_max = RequiredDouble(table, "up_max", prefix + ".up_max");
    quantile.down_min = RequiredDouble(table, "down_min", prefix + ".down_min");
    quantile.down_max = RequiredDouble(table, "down_max", prefix + ".down_max");
    quantile.precision =
        RequiredDouble(table, "precision", prefix + ".precision");
    if (!ok_) {
      return quantile;
    }
    if (quantile.move <= 0.0 || quantile.move >= 1.0) {
      Fail(prefix + ".move", " must be between 0 and 1");
      return quantile;
    }
    if (quantile.up_max <= quantile.up_min) {
      Fail(prefix + ".up_max", " must be greater than up_min");
      return quantile;
    }
    if (quantile.down_max <= quantile.down_min) {
      Fail(prefix + ".down_max", " must be greater than down_min");
      return quantile;
    }
    if (quantile.precision <= 0.0) {
      Fail(prefix + ".precision", " must be positive");
      return quantile;
    }
    quantile.up_bins = RequiredBins(quantile.up_min, quantile.up_max,
                                    quantile.precision, prefix + ".up");
    quantile.down_bins = RequiredBins(quantile.down_min, quantile.down_max,
                                      quantile.precision, prefix + ".down");
    return quantile;
  }

  [[nodiscard]] ExecuteConfig ParseExecute(const toml::table& table,
                                           const std::string& prefix) {
    ExecuteConfig execute;
    execute.open_notional =
        RequiredDouble(table, "open_notional", prefix + ".open_notional");
    execute.trailing_stop =
        RequiredDouble(table, "trailing_stop", prefix + ".trailing_stop");
    execute.max_entry_spread =
        RequiredDouble(table, "max_entry_spread", prefix + ".max_entry_spread");
    execute.parallel = RequiredUInt32(table, "parallel", prefix + ".parallel");
    if (!ok_) {
      return execute;
    }
    if (execute.open_notional <= 0.0) {
      Fail(prefix + ".open_notional", " must be positive");
      return execute;
    }
    if (execute.parallel == 0) {
      Fail(prefix + ".parallel", " must be positive");
    }
    return execute;
  }

  [[nodiscard]] RiskConfig ParseRisk(const toml::table& table) {
    RiskConfig risk;
    risk.max_gross_notional =
        RequiredDouble(table, "max_gross_notional",
                       "lead_lag.risk.max_gross_notional");
    risk.max_holding_position =
        RequiredInt64(table, "max_holding_position",
                      "lead_lag.risk.max_holding_position");
    if (!ok_) {
      return risk;
    }
    if (risk.max_gross_notional <= 0.0) {
      Fail("lead_lag.risk.max_gross_notional", " must be positive");
      return risk;
    }
    if (risk.max_holding_position <= 0) {
      Fail("lead_lag.risk.max_holding_position", " must be positive");
    }
    return risk;
  }

  [[nodiscard]] BboRecordConfig ParseBboRecord(const toml::table& table,
                                               const std::string& prefix) {
    return BboRecordConfig{
        .window_ns = RequiredDurationNs(table, "window", prefix + ".window"),
        .stats_window_ns =
            RequiredDurationNs(table, "stats_window", prefix + ".stats_window"),
    };
  }

  [[nodiscard]] CapacityConfig ParseCapacity(const toml::table& table,
                                             const std::string& prefix) {
    CapacityConfig capacity;
    capacity.extrema_window_capacity = SizeOr(
        table, "extrema_window_capacity", capacity.extrema_window_capacity,
        prefix + ".extrema_window_capacity");
    capacity.move_queue_capacity =
        SizeOr(table, "move_queue_capacity", capacity.move_queue_capacity,
               prefix + ".move_queue_capacity");
    capacity.noise_window_capacity =
        SizeOr(table, "noise_window_capacity", capacity.noise_window_capacity,
               prefix + ".noise_window_capacity");
    capacity.spread_window_capacity =
        SizeOr(table, "spread_window_capacity", capacity.spread_window_capacity,
               prefix + ".spread_window_capacity");
    return capacity;
  }

  [[nodiscard]] bool LoadInstrumentMetadata(PairConfig* pair,
                                            std::size_t index) {
    const std::string prefix = PairName(index, "");
    const config::InstrumentInfo* lead =
        catalog_.Find(pair->lead_exchange, pair->symbol);
    if (lead == nullptr) {
      Fail(prefix + ".symbol", " is missing lead instrument metadata");
      return false;
    }
    const config::InstrumentInfo* lag =
        catalog_.Find(pair->lag_exchange, pair->symbol);
    if (lag == nullptr) {
      Fail(prefix + ".symbol", " is missing lag instrument metadata");
      return false;
    }
    if (lead->symbol_id != pair->symbol_id ||
        lag->symbol_id != pair->symbol_id) {
      Fail(prefix + ".symbol_id", " does not match instrument catalog");
      return false;
    }
    if (!lag->quantity_step || !lag->quantity_decimal_places) {
      Fail(prefix + ".symbol", " lag instrument quantity metadata is required");
      return false;
    }
    if (lag->price_tick <= 0.0 || *lag->quantity_step <= 0.0 ||
        lag->notional_multiplier <= 0.0) {
      Fail(prefix + ".symbol", " lag instrument trading metadata is invalid");
      return false;
    }

    pair->lag_instrument = InstrumentMetadata{
        .symbol_id = lag->symbol_id,
        .exchange = lag->exchange,
        .exchange_symbol = lag->exchange_symbol,
        .price_tick = lag->price_tick,
        .price_decimal_places = lag->price_decimal_places,
        .quantity_step = *lag->quantity_step,
        .quantity_decimal_places = *lag->quantity_decimal_places,
        .min_quantity = lag->min_quantity,
        .max_quantity = lag->max_quantity,
        .notional_multiplier = lag->notional_multiplier,
        .lag_taker_fee = pair->lag_taker_fee,
    };
    return true;
  }

  [[nodiscard]] std::string RequiredString(const toml::table& table,
                                           std::string_view key,
                                           std::string_view name) {
    const std::optional<std::string> value = table[key].value<std::string>();
    if (!value || value->empty()) {
      Fail(name, " is required");
      return {};
    }
    return *value;
  }

  [[nodiscard]] double RequiredDouble(const toml::table& table,
                                      std::string_view key,
                                      std::string_view name) {
    const std::optional<double> value = table[key].value<double>();
    if (!value) {
      Fail(name, " is required");
      return 0.0;
    }
    return *value;
  }

  [[nodiscard]] std::int32_t RequiredInt32(const toml::table& table,
                                           std::string_view key,
                                           std::string_view name) {
    const std::optional<std::int64_t> value = table[key].value<std::int64_t>();
    if (!value) {
      Fail(name, " is required");
      return 0;
    }
    if (*value < std::numeric_limits<std::int32_t>::min() ||
        *value > std::numeric_limits<std::int32_t>::max()) {
      Fail(name, " exceeds int32 range");
      return 0;
    }
    return static_cast<std::int32_t>(*value);
  }

  [[nodiscard]] std::int64_t RequiredInt64(const toml::table& table,
                                           std::string_view key,
                                           std::string_view name) {
    const std::optional<std::int64_t> value = table[key].value<std::int64_t>();
    if (!value) {
      Fail(name, " is required");
      return 0;
    }
    return *value;
  }

  [[nodiscard]] std::uint32_t RequiredUInt32(const toml::table& table,
                                             std::string_view key,
                                             std::string_view name) {
    const std::optional<std::int64_t> value = table[key].value<std::int64_t>();
    if (!value) {
      Fail(name, " is required");
      return 0;
    }
    if (*value < 0 || static_cast<std::uint64_t>(*value) >
                          std::numeric_limits<std::uint32_t>::max()) {
      Fail(name, " exceeds uint32 range");
      return 0;
    }
    return static_cast<std::uint32_t>(*value);
  }

  [[nodiscard]] std::size_t SizeOr(const toml::table& table,
                                   std::string_view key, std::size_t fallback,
                                   std::string_view name) {
    const std::optional<std::int64_t> value = table[key].value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value <= 0 || static_cast<std::uint64_t>(*value) >
                           std::numeric_limits<std::size_t>::max()) {
      Fail(name, " must be positive");
      return fallback;
    }
    return static_cast<std::size_t>(*value);
  }

  [[nodiscard]] Exchange RequiredExchange(const toml::table& table,
                                          std::string_view key,
                                          std::string_view name) {
    const std::string text = RequiredString(table, key, name);
    Exchange exchange{};
    if (ok_ && !ParseExchange(text, &exchange)) {
      Fail(name, " is unsupported");
    }
    return exchange;
  }

  [[nodiscard]] std::uint64_t RequiredDurationNs(const toml::table& table,
                                                 std::string_view key,
                                                 std::string_view name) {
    const std::string text = RequiredString(table, key, name);
    if (!ok_) {
      return 0;
    }
    return ParseDurationNs(text, name);
  }

  [[nodiscard]] std::uint64_t ParseDurationNs(std::string_view text,
                                              std::string_view name) {
    std::uint64_t value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr == begin) {
      Fail(name, " must be an integer duration with unit");
      return 0;
    }
    const std::string_view unit{parsed.ptr,
                                static_cast<std::size_t>(end - parsed.ptr)};
    std::uint64_t multiplier = 0;
    if (unit == "ns") {
      multiplier = 1;
    } else if (unit == "us") {
      multiplier = 1'000ULL;
    } else if (unit == "ms") {
      multiplier = 1'000'000ULL;
    } else if (unit == "s") {
      multiplier = 1'000'000'000ULL;
    } else if (unit == "m") {
      multiplier = 60'000'000'000ULL;
    } else if (unit == "h") {
      multiplier = 3'600'000'000'000ULL;
    } else {
      Fail(name, " unit must be ns, us, ms, s, m, or h");
      return 0;
    }
    if (value == 0) {
      Fail(name, " must be positive");
      return 0;
    }
    if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
      Fail(name, " exceeds uint64 nanosecond range");
      return 0;
    }
    return value * multiplier;
  }

  [[nodiscard]] std::size_t RequiredBins(double min_value, double max_value,
                                         double precision,
                                         const std::string& name) {
    const double bins = std::ceil((max_value - min_value) / precision);
    if (bins <= 0.0 ||
        bins > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
      Fail(name, " range and precision produce invalid bins");
      return 0;
    }
    return static_cast<std::size_t>(bins);
  }

  void Fail(std::string_view name, std::string_view message) {
    ok_ = false;
    error_.assign(name);
    error_.append(message);
  }

  [[nodiscard]] std::string PairName(std::size_t index,
                                     std::string_view suffix) const {
    return fmt::format("lead_lag.pairs[{}]{}", index, suffix);
  }

  const toml::table& node_;
  const config::InstrumentCatalog& catalog_;
  Config config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

double ExecuteConfig::EntrySpreadLimit() const noexcept {
  if (max_entry_spread < 0.0) {
    return trailing_stop;
  }
  return max_entry_spread;
}

bool RiskConfig::GrossNotionalLimitEnabled() const noexcept {
  return max_gross_notional > 0.0;
}

bool RiskConfig::HoldingPositionLimitEnabled() const noexcept {
  return max_holding_position > 0;
}

ConfigResult ParseConfig(const toml::table& node,
                         const config::InstrumentCatalog& catalog) {
  return Parser{node, catalog}.Parse();
}

ConfigResult LoadConfigFile(const std::filesystem::path& path,
                            const config::InstrumentCatalog& catalog) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseConfig(parsed, catalog);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load lead_lag config: "} +
                   exc.what());
  }
}

}  // namespace aquila::strategy::leadlag
