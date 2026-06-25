#ifndef AQUILA_STRATEGY_LEAD_LAG_DRIFT_GUARD_H_
#define AQUILA_STRATEGY_LEAD_LAG_DRIFT_GUARD_H_

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/types.h"
#include "strategy/lead_lag/window_stats.h"

namespace aquila::strategy::leadlag {

struct DriftGuardSnapshot {
  bool enabled{false};
  bool ready{false};
  double instant_ratio{0.0};
  bool instant_hit{false};
  double ratio_std{0.0};
  bool ratio_std_hit{false};
  double drift_mean{0.0};
  bool drift_mean_hit{false};
  bool blocked{false};
};

class DriftGuardState {
 public:
  void Init(const DriftGuardConfig& config, std::size_t initial_capacity) {
    enabled_ = config.enabled;
    ratio_std_.Init(config.ratio_std_window_ns, initial_capacity);
    ratio_mean_.Init(config.drift_mean_window_ns, initial_capacity);
    Reset();
  }

  void Reset() noexcept {
    ratio_std_.Clear();
    ratio_mean_.Clear();
  }

  void OnPairedRawBbo(std::int64_t event_ns, const QuoteSnapshot& lead,
                      const QuoteSnapshot& lag) {
    if (!enabled_) {
      return;
    }

    double ratio = 0.0;
    if (!ComputeRatio(lead, lag, &ratio)) {
      return;
    }
    ratio_std_.Update(event_ns, ratio);
    ratio_mean_.Update(event_ns, ratio);
  }

  [[nodiscard]] DriftGuardSnapshot Evaluate(
      const DriftGuardConfig& config, const QuoteSnapshot& lead,
      const QuoteSnapshot& lag) const noexcept {
    DriftGuardSnapshot snapshot{
        .enabled = config.enabled,
    };
    if (!config.enabled || !enabled_ || ratio_std_.empty() ||
        ratio_mean_.empty()) {
      return snapshot;
    }

    double ratio = 0.0;
    if (!ComputeRatio(lead, lag, &ratio)) {
      return snapshot;
    }

    snapshot.ready = true;
    snapshot.instant_ratio = ratio;
    snapshot.instant_hit =
        std::abs(snapshot.instant_ratio - 1.0) > config.drift_instant;
    snapshot.ratio_std = ratio_std_.stddev();
    snapshot.ratio_std_hit = snapshot.ratio_std > config.ratio_std;
    snapshot.drift_mean = ratio_mean_.mean();
    snapshot.drift_mean_hit =
        std::abs(snapshot.drift_mean - 1.0) > config.drift_mean;
    snapshot.blocked = snapshot.instant_hit || snapshot.ratio_std_hit ||
                       snapshot.drift_mean_hit;
    return snapshot;
  }

 private:
  [[nodiscard]] static bool ComputeRatio(const QuoteSnapshot& lead,
                                         const QuoteSnapshot& lag,
                                         double* ratio) noexcept {
    if (!ValidPrice(lead.bid_price) || !ValidPrice(lead.ask_price) ||
        !ValidPrice(lag.bid_price) || !ValidPrice(lag.ask_price)) {
      return false;
    }
    const double lead_mid = (lead.bid_price + lead.ask_price) * 0.5;
    const double lag_mid = (lag.bid_price + lag.ask_price) * 0.5;
    if (!std::isfinite(lead_mid) || !std::isfinite(lag_mid) ||
        lead_mid <= 0.0 || lag_mid <= 0.0) {
      return false;
    }
    *ratio = lag_mid / lead_mid;
    return std::isfinite(*ratio);
  }

  [[nodiscard]] static bool ValidPrice(double price) noexcept {
    return std::isfinite(price) && price > 0.0;
  }

  MeanStdWindow ratio_std_;
  MeanWindow ratio_mean_;
  bool enabled_{false};
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_DRIFT_GUARD_H_
