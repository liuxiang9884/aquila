#ifndef AQUILA_STRATEGY_LEAD_LAG_CONFIG_H_
#define AQUILA_STRATEGY_LEAD_LAG_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/common/types.h"
#include "core/config/instrument_catalog.h"

namespace aquila::strategy::leadlag {

inline constexpr std::size_t kDefaultWindowCapacity = 16 * 1024;
inline constexpr std::size_t kDefaultQuantileBinCount = 4096;
inline constexpr std::uint64_t kDefaultMaxLeadFreshnessNs = 5'000'000ULL;
inline constexpr std::uint64_t kDefaultMaxLagFreshnessNs = 20'000'000ULL;

struct QuantileConfig {
  double move{0.0};
  double up_min{0.0};
  double up_max{0.0};
  double down_min{0.0};
  double down_max{0.0};
  double precision{0.0};
  std::size_t up_bins{kDefaultQuantileBinCount};
  std::size_t down_bins{kDefaultQuantileBinCount};
};

struct TriggerConfig {
  double lead{0.0};
  double close{0.0};
  double lag_part{0.0};
  double target_profit_rate{0.0};
  double drift_limit{0.0};
  std::uint64_t drift_period_ns{0};
  std::uint32_t drift_min_samples{0};
  std::uint64_t drift_warmup_ns{0};
  QuantileConfig quantile;
};

struct ExecuteConfig {
  double open_notional{0.0};
  double trailing_stop{0.0};
  double max_entry_spread{-1.0};
  std::uint32_t open_slippage{0};
  std::uint32_t close_slippage{0};
  std::uint32_t parallel{1};

  [[nodiscard]] double EntrySpreadLimit() const noexcept;
};

struct RiskConfig {
  double max_gross_notional{0.0};
  double max_holding_position{0.0};

  [[nodiscard]] bool GrossNotionalLimitEnabled() const noexcept;
  [[nodiscard]] bool HoldingPositionLimitEnabled() const noexcept;
};

struct FreshnessConfig {
  std::uint64_t max_lead_freshness_ns{kDefaultMaxLeadFreshnessNs};
  std::uint64_t max_lag_freshness_ns{kDefaultMaxLagFreshnessNs};
};

struct BboRecordConfig {
  std::uint64_t window_ns{0};
  std::uint64_t stats_window_ns{0};
};

struct CapacityConfig {
  std::size_t extrema_window_capacity{kDefaultWindowCapacity};
  std::size_t move_queue_capacity{kDefaultWindowCapacity};
  std::size_t noise_window_capacity{kDefaultWindowCapacity};
  std::size_t spread_window_capacity{kDefaultWindowCapacity};
};

struct InstrumentMetadata {
  std::int32_t symbol_id{-1};
  Exchange exchange{Exchange::kGate};
  std::string exchange_symbol;
  double price_tick{0.0};
  std::int32_t price_decimal_places{0};
  double quantity_step{0.0};
  std::int32_t quantity_decimal_places{0};
  double min_quantity{0.0};
  double max_quantity{0.0};
  double notional_multiplier{0.0};
  double lag_taker_fee{0.0};
};

struct PairConfig {
  std::string symbol;
  std::int32_t symbol_id{-1};
  Exchange lead_exchange{Exchange::kBinance};
  Exchange lag_exchange{Exchange::kGate};
  double lag_taker_fee{0.0};
  TriggerConfig trigger;
  ExecuteConfig execute;
  BboRecordConfig bbo_record;
  CapacityConfig capacity;
  InstrumentMetadata lag_instrument;
};

struct Config {
  std::string name;
  std::string version;
  FreshnessConfig freshness;
  RiskConfig risk;
  std::vector<PairConfig> pairs;
};

using ConfigResult = Result<Config>;

[[nodiscard]] ConfigResult ParseConfig(
    const toml::table& node, const config::InstrumentCatalog& catalog);

[[nodiscard]] ConfigResult LoadConfigFile(
    const std::filesystem::path& path,
    const config::InstrumentCatalog& catalog);

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_CONFIG_H_
