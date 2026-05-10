#ifndef AQUILA_STRATEGY_LEAD_LAG_RECORDERS_H_
#define AQUILA_STRATEGY_LEAD_LAG_RECORDERS_H_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "core/base/histogram_quantile.h"
#include "core/base/monotonic_deque.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/types.h"
#include "strategy/lead_lag/window_stats.h"

namespace aquila::strategy::leadlag {

struct RecorderStats {
  std::uint64_t extrema_capacity_grow_count{0};
};

struct BboExtremaSnapshot {
  bool valid{false};
  double bid_min{0.0};
  double bid_max{0.0};
  double ask_min{0.0};
  double ask_max{0.0};
};

namespace recorder_detail {

struct PricePoint {
  std::int64_t local_ns{0};
  double price{0.0};
};

struct PriceLess {
  [[nodiscard]] bool operator()(const PricePoint& lhs,
                                const PricePoint& rhs) const noexcept {
    return lhs.price < rhs.price;
  }
};

struct PriceGreater {
  [[nodiscard]] bool operator()(const PricePoint& lhs,
                                const PricePoint& rhs) const noexcept {
    return lhs.price > rhs.price;
  }
};

template <typename Compare>
void PushAndTrackGrow(MonotonicDeque<PricePoint, Compare>* deque,
                      const PricePoint& point, RecorderStats* stats) {
  const std::size_t old_capacity = deque->capacity();
  deque->Push(point);
  if (stats != nullptr && deque->capacity() > old_capacity) {
    ++stats->extrema_capacity_grow_count;
  }
}

template <typename Compare>
void EvictExpired(MonotonicDeque<PricePoint, Compare>* deque,
                  std::int64_t now_ns, std::uint64_t window_ns) {
  while (!deque->empty()) {
    const PricePoint& oldest = deque->Front();
    if (now_ns < oldest.local_ns ||
        static_cast<std::uint64_t>(now_ns - oldest.local_ns) <= window_ns) {
      break;
    }
    deque->PopFront();
  }
}

[[nodiscard]] inline std::int64_t NextRollAt(std::int64_t now_ns,
                                             std::uint64_t window_ns) noexcept {
  assert(window_ns > 0);
  const auto window = static_cast<std::int64_t>(window_ns);
  return (now_ns / window) * window + window;
}

}  // namespace recorder_detail

class BboExtremaWindow {
 public:
  void Init(std::uint64_t window_ns, std::size_t capacity,
            RecorderStats* stats = nullptr) {
    window_ns_ = window_ns;
    stats_ = stats;
    bid_min_.Reserve(capacity);
    bid_max_.Reserve(capacity);
    ask_min_.Reserve(capacity);
    ask_max_.Reserve(capacity);
    Reset();
  }

  void Reset() noexcept {
    bid_min_.Clear();
    bid_max_.Clear();
    ask_min_.Clear();
    ask_max_.Clear();
  }

  void Update(const QuoteSnapshot& quote) {
    recorder_detail::PushAndTrackGrow(
        &bid_min_,
        recorder_detail::PricePoint{.local_ns = quote.local_ns,
                                    .price = quote.bid_price},
        stats_);
    recorder_detail::PushAndTrackGrow(
        &bid_max_,
        recorder_detail::PricePoint{.local_ns = quote.local_ns,
                                    .price = quote.bid_price},
        stats_);
    recorder_detail::PushAndTrackGrow(
        &ask_min_,
        recorder_detail::PricePoint{.local_ns = quote.local_ns,
                                    .price = quote.ask_price},
        stats_);
    recorder_detail::PushAndTrackGrow(
        &ask_max_,
        recorder_detail::PricePoint{.local_ns = quote.local_ns,
                                    .price = quote.ask_price},
        stats_);
    EvictExpired(quote.local_ns);
  }

  [[nodiscard]] BboExtremaSnapshot snapshot() const noexcept {
    if (bid_min_.empty() || bid_max_.empty() || ask_min_.empty() ||
        ask_max_.empty()) {
      return BboExtremaSnapshot{};
    }
    return BboExtremaSnapshot{
        .valid = true,
        .bid_min = bid_min_.Front().price,
        .bid_max = bid_max_.Front().price,
        .ask_min = ask_min_.Front().price,
        .ask_max = ask_max_.Front().price,
    };
  }

 private:
  void EvictExpired(std::int64_t now_ns) {
    recorder_detail::EvictExpired(&bid_min_, now_ns, window_ns_);
    recorder_detail::EvictExpired(&bid_max_, now_ns, window_ns_);
    recorder_detail::EvictExpired(&ask_min_, now_ns, window_ns_);
    recorder_detail::EvictExpired(&ask_max_, now_ns, window_ns_);
  }

  std::uint64_t window_ns_{0};
  RecorderStats* stats_{nullptr};
  MonotonicDeque<recorder_detail::PricePoint, recorder_detail::PriceLess>
      bid_min_;
  MonotonicDeque<recorder_detail::PricePoint, recorder_detail::PriceGreater>
      bid_max_;
  MonotonicDeque<recorder_detail::PricePoint, recorder_detail::PriceLess>
      ask_min_;
  MonotonicDeque<recorder_detail::PricePoint, recorder_detail::PriceGreater>
      ask_max_;
};

class NoiseState {
 public:
  void Init(std::uint64_t mid_window_ns, std::uint64_t ratio_window_ns,
            std::size_t capacity) {
    mid_window_.Init(mid_window_ns, capacity);
    ratio_window_.Init(ratio_window_ns, capacity);
  }

  void Clear() noexcept {
    mid_window_.Clear();
    ratio_window_.Clear();
  }

  void Update(const QuoteSnapshot& quote) {
    const double mid = (quote.bid_price + quote.ask_price) * 0.5;
    mid_window_.Update(quote.local_ns, mid);
    const double mean = mid_window_.mean();
    if (mean != 0.0) {
      ratio_window_.Update(quote.local_ns, mid_window_.stddev() / mean);
    }
  }

  [[nodiscard]] double value() const noexcept {
    return ratio_window_.mean();
  }

 private:
  MeanStdWindow mid_window_;
  MeanWindow ratio_window_;
};

class SpreadState {
 public:
  void Init(std::uint64_t window_ns, std::size_t capacity) {
    spread_window_.Init(window_ns, capacity);
  }

  void Clear() noexcept {
    spread_window_.Clear();
  }

  void Update(const QuoteSnapshot& quote) {
    spread_window_.Update(quote.local_ns, quote.ask_price - quote.bid_price);
  }

  [[nodiscard]] double mean() const noexcept {
    return spread_window_.mean();
  }

  [[nodiscard]] double buffer(double current_spread) const noexcept {
    return std::max(current_spread - mean(), 0.0);
  }

  [[nodiscard]] double buffer(const QuoteSnapshot& quote) const noexcept {
    return buffer(quote.ask_price - quote.bid_price);
  }

 private:
  MeanWindow spread_window_;
};

struct MoveQuantileRoll {
  bool rolled{false};
  double up_quantile{0.0};
  double down_quantile{0.0};
  std::int64_t roll_at_ns{0};
};

class MoveQuantileWindow {
 public:
  void Init(std::int64_t start_ns, std::uint64_t stats_window_ns,
            const QuantileConfig& quantile) {
    stats_window_ns_ = stats_window_ns;
    up_histogram_.Init(quantile.up_min, quantile.up_max, quantile.up_bins,
                       quantile.move, HistogramQuantileValueMode::kUpperEdge);
    down_histogram_.Init(quantile.down_min, quantile.down_max,
                         quantile.down_bins, 1.0 - quantile.move,
                         HistogramQuantileValueMode::kLowerEdge);
    Reset(start_ns);
  }

  void Reset(std::int64_t start_ns) noexcept {
    up_histogram_.Reset();
    down_histogram_.Reset();
    roll_at_ns_ = recorder_detail::NextRollAt(start_ns, stats_window_ns_);
  }

  [[nodiscard]] MoveQuantileRoll Update(
      const QuoteSnapshot& lead, const BboExtremaSnapshot& extrema) noexcept {
    MoveQuantileRoll roll = RollIfNeeded(lead.local_ns);
    if (extrema.valid) {
      up_histogram_.Add(lead.bid_price / extrema.bid_min - 1.0);
      down_histogram_.Add(lead.ask_price / extrema.ask_max - 1.0);
    }
    return roll;
  }

  [[nodiscard]] MoveQuantileRoll RollIfNeeded(std::int64_t now_ns) noexcept {
    MoveQuantileRoll roll{
        .rolled = false,
        .up_quantile = 0.0,
        .down_quantile = 0.0,
        .roll_at_ns = roll_at_ns_,
    };
    if (now_ns <= roll_at_ns_) {
      return roll;
    }

    roll.rolled = true;
    roll.up_quantile = up_histogram_.Value();
    roll.down_quantile = down_histogram_.Value();
    up_histogram_.Reset();
    down_histogram_.Reset();
    roll_at_ns_ = recorder_detail::NextRollAt(now_ns, stats_window_ns_);
    roll.roll_at_ns = roll_at_ns_;
    return roll;
  }

  [[nodiscard]] std::uint64_t sample_count() const noexcept {
    return up_histogram_.count();
  }

  [[nodiscard]] std::int64_t roll_at_ns() const noexcept {
    return roll_at_ns_;
  }

 private:
  std::uint64_t stats_window_ns_{0};
  std::int64_t roll_at_ns_{0};
  HistogramQuantile<double> up_histogram_;
  HistogramQuantile<double> down_histogram_;
};

struct RecorderSnapshot {
  BboExtremaSnapshot lead_extrema;
  BboExtremaSnapshot lag_extrema;
  double lead_noise{0.0};
  double lag_noise{0.0};
  double lag_spread_mean{0.0};
};

class RecorderState {
 public:
  void Init(const PairConfig& pair) {
    bbo_window_ns_ = pair.bbo_record.window_ns;
    stats_window_ns_ = pair.bbo_record.stats_window_ns;
    quantile_ = pair.trigger.quantile;
    lead_extrema_.Init(bbo_window_ns_, pair.capacity.extrema_window_capacity,
                       &stats_);
    lag_extrema_.Init(bbo_window_ns_, pair.capacity.extrema_window_capacity,
                      &stats_);
    lead_noise_.Init(bbo_window_ns_, stats_window_ns_,
                     pair.capacity.noise_window_capacity);
    lag_noise_.Init(bbo_window_ns_, stats_window_ns_,
                    pair.capacity.noise_window_capacity);
    lag_spread_.Init(stats_window_ns_, pair.capacity.spread_window_capacity);
    move_quantile_.Init(/*start_ns=*/0, stats_window_ns_, quantile_);
  }

  void SeedActive(const QuoteSnapshot& lead_seed,
                  const QuoteSnapshot& lag_seed) {
    lead_extrema_.Reset();
    lag_extrema_.Reset();
    lead_noise_.Clear();
    lag_noise_.Clear();
    lag_spread_.Clear();
    move_quantile_.Reset(lead_seed.local_ns);

    lead_extrema_.Update(lead_seed);
    lead_noise_.Update(lead_seed);
    lag_extrema_.Update(lag_seed);
    lag_spread_.Update(lag_seed);
    lag_noise_.Update(lag_seed);
  }

  [[nodiscard]] MoveQuantileRoll OnLeadActiveTick(
      const QuoteSnapshot& drifted_lead) {
    lead_extrema_.Update(drifted_lead);
    lead_noise_.Update(drifted_lead);
    return move_quantile_.Update(drifted_lead, lead_extrema_.snapshot());
  }

  void OnLagActiveTick(const QuoteSnapshot& lag) {
    lag_extrema_.Update(lag);
    lag_spread_.Update(lag);
    lag_noise_.Update(lag);
  }

  [[nodiscard]] RecorderSnapshot snapshot() const noexcept {
    return RecorderSnapshot{
        .lead_extrema = lead_extrema_.snapshot(),
        .lag_extrema = lag_extrema_.snapshot(),
        .lead_noise = lead_noise_.value(),
        .lag_noise = lag_noise_.value(),
        .lag_spread_mean = lag_spread_.mean(),
    };
  }

  [[nodiscard]] const RecorderStats& stats() const noexcept {
    return stats_;
  }

  [[nodiscard]] MoveQuantileWindow& move_quantile() noexcept {
    return move_quantile_;
  }

  [[nodiscard]] const MoveQuantileWindow& move_quantile() const noexcept {
    return move_quantile_;
  }

 private:
  std::uint64_t bbo_window_ns_{0};
  std::uint64_t stats_window_ns_{0};
  QuantileConfig quantile_;
  RecorderStats stats_;
  BboExtremaWindow lead_extrema_;
  BboExtremaWindow lag_extrema_;
  NoiseState lead_noise_;
  NoiseState lag_noise_;
  SpreadState lag_spread_;
  MoveQuantileWindow move_quantile_;
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_RECORDERS_H_
