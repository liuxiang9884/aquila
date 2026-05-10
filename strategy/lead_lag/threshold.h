#ifndef AQUILA_STRATEGY_LEAD_LAG_THRESHOLD_H_
#define AQUILA_STRATEGY_LEAD_LAG_THRESHOLD_H_

#include <cstdint>

#include "strategy/lead_lag/alignment.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/recorders.h"

namespace aquila::strategy::leadlag {

struct ThresholdSnapshot {
  bool initialized{false};
  double up_entry{0.0};
  double down_entry{0.0};
  double up_exit{0.0};
  double down_exit{0.0};
  double lead_noise{0.0};
  double lag_noise{0.0};
  double drift_std_ema{0.0};
  double last_up_quantile{0.0};
  double last_down_quantile{0.0};
  double last_profit_buffer{0.0};
  std::uint64_t roll_count{0};
  std::int64_t last_roll_at_ns{0};
};

class ThresholdState {
 public:
  void Init(const PairConfig& pair) noexcept {
    lag_taker_fee_ = pair.lag_taker_fee;
    snapshot_ = ThresholdSnapshot{
        .initialized = true,
        .up_entry = pair.trigger.lead,
        .down_entry = -pair.trigger.lead,
        .up_exit = pair.trigger.close,
        .down_exit = -pair.trigger.close,
    };
  }

  [[nodiscard]] ThresholdSnapshot OnMoveRoll(
      const MoveQuantileRoll& roll, const RecorderSnapshot& recorder,
      const AlignmentSnapshot& alignment) noexcept {
    if (!roll.rolled) {
      return snapshot_;
    }

    const double exit = alignment.drift_std_ema;
    const double profit_buffer =
        lag_taker_fee_ * 2.0 + recorder.lead_noise + recorder.lag_noise;

    snapshot_.lead_noise = recorder.lead_noise;
    snapshot_.lag_noise = recorder.lag_noise;
    snapshot_.drift_std_ema = exit;
    snapshot_.last_up_quantile = roll.up_quantile;
    snapshot_.last_down_quantile = roll.down_quantile;
    snapshot_.last_profit_buffer = profit_buffer;
    snapshot_.last_roll_at_ns = roll.roll_at_ns;
    ++snapshot_.roll_count;

    snapshot_.up_exit = exit;
    if (roll.up_quantile - exit > profit_buffer) {
      snapshot_.up_entry = roll.up_quantile;
    } else {
      snapshot_.up_entry = exit + profit_buffer;
    }

    snapshot_.down_exit = -exit;
    if (-roll.down_quantile - exit > profit_buffer) {
      snapshot_.down_entry = roll.down_quantile;
    } else {
      snapshot_.down_entry = -exit - profit_buffer;
    }
    return snapshot_;
  }

  [[nodiscard]] const ThresholdSnapshot& snapshot() const noexcept {
    return snapshot_;
  }

 private:
  double lag_taker_fee_{0.0};
  ThresholdSnapshot snapshot_;
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_THRESHOLD_H_
