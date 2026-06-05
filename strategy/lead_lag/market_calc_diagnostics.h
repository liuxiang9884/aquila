#ifndef AQUILA_STRATEGY_LEAD_LAG_MARKET_CALC_DIAGNOSTICS_H_
#define AQUILA_STRATEGY_LEAD_LAG_MARKET_CALC_DIAGNOSTICS_H_

#include <cstdint>
#include <limits>
#include <string_view>

#include "core/common/types.h"
#include "strategy/lead_lag/types.h"

namespace aquila::strategy::leadlag {

[[nodiscard]] inline double MarketCalcUnavailable() noexcept {
  return std::numeric_limits<double>::quiet_NaN();
}

struct MarketCalcRow {
  std::uint64_t row_index{0};
  PairRole role{PairRole::kNone};
  std::string_view symbol;
  std::int32_t symbol_id{0};
  std::int64_t book_ticker_id{0};
  Exchange exchange{Exchange::kBinance};
  std::int64_t exchange_ns{0};
  std::int64_t local_ns{0};
  std::int64_t event_ns{0};
  bool price_changed{false};
  bool both_sides_valid{false};
  bool active{false};
  double lead_bid{MarketCalcUnavailable()};
  double lead_ask{MarketCalcUnavailable()};
  double lag_bid{MarketCalcUnavailable()};
  double lag_ask{MarketCalcUnavailable()};
  double drift_mean{MarketCalcUnavailable()};
  double drift_std_ema{MarketCalcUnavailable()};
  double drifted_lead_bid{MarketCalcUnavailable()};
  double drifted_lead_ask{MarketCalcUnavailable()};
  double up_entry{MarketCalcUnavailable()};
  double down_entry{MarketCalcUnavailable()};
  double up_exit{MarketCalcUnavailable()};
  double down_exit{MarketCalcUnavailable()};
  double lead_noise{MarketCalcUnavailable()};
  double lag_noise{MarketCalcUnavailable()};
  double lag_spread_mean{MarketCalcUnavailable()};
  double long_lead_move{MarketCalcUnavailable()};
  double long_price_diff{MarketCalcUnavailable()};
  double long_lag_part_ratio{MarketCalcUnavailable()};
  double long_target_space{MarketCalcUnavailable()};
  double long_required_edge{MarketCalcUnavailable()};
  double short_lead_move{MarketCalcUnavailable()};
  double short_price_diff{MarketCalcUnavailable()};
  double short_lag_part_ratio{MarketCalcUnavailable()};
  double short_target_space{MarketCalcUnavailable()};
  double short_required_edge{MarketCalcUnavailable()};
  double lag_spread{MarketCalcUnavailable()};
  double lag_spread_buffer{MarketCalcUnavailable()};
  double lag_spread_pct{MarketCalcUnavailable()};
};

using MarketCalcObserver = void (*)(void* context,
                                    const MarketCalcRow& row) noexcept;

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_MARKET_CALC_DIAGNOSTICS_H_
