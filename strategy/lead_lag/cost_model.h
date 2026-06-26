#ifndef AQUILA_STRATEGY_LEAD_LAG_COST_MODEL_H_
#define AQUILA_STRATEGY_LEAD_LAG_COST_MODEL_H_

#include <algorithm>

#include "strategy/lead_lag/types.h"

namespace aquila::strategy::leadlag {

struct EntryCostBreakdown {
  double fee{0.0};
  double spread{0.0};
  double lag_spread_buffer{0.0};
  double entry_slippage_buffer{0.0};
  double normal_close_slippage_buffer{0.0};
  double lead_noise{0.0};
  double lag_noise{0.0};

  [[nodiscard]] double RequiredEdge() const noexcept {
    return fee + entry_slippage_buffer + normal_close_slippage_buffer +
           lead_noise + lag_noise;
  }

  [[nodiscard]] double RequiredEdgeWithTargetProfit(
      double target_profit_rate) const noexcept {
    return RequiredEdge() + target_profit_rate;
  }

  [[nodiscard]] double EmbeddedPriceFriction() const noexcept {
    return spread + lag_spread_buffer;
  }
};

[[nodiscard]] inline double SpreadPct(const QuoteSnapshot& quote) noexcept {
  return (quote.ask_price - quote.bid_price) /
         (quote.ask_price + quote.bid_price) * 2.0;
}

[[nodiscard]] inline double LagSpreadBuffer(const QuoteSnapshot& lag,
                                            double lag_spread_mean) noexcept {
  return std::max((lag.ask_price - lag.bid_price) - lag_spread_mean, 0.0);
}

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_COST_MODEL_H_
