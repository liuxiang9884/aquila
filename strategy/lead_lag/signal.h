#ifndef AQUILA_STRATEGY_LEAD_LAG_SIGNAL_H_
#define AQUILA_STRATEGY_LEAD_LAG_SIGNAL_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core/common/types.h"
#include "strategy/lead_lag/alignment.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/cost_model.h"
#include "strategy/lead_lag/execution_state.h"
#include "strategy/lead_lag/recorders.h"
#include "strategy/lead_lag/threshold.h"
#include "strategy/lead_lag/types.h"

namespace aquila::strategy::leadlag {

enum class SignalAction : std::uint8_t {
  kNone,
  kOpenLong,
  kOpenShort,
  kCloseLong,
  kCloseShort,
  kStoplossLong,
  kStoplossShort,
};

enum class SignalRejectReason : std::uint8_t {
  kNone,
  kInvalidState,
  kPriceDiff,
  kThreshold,
  kLagPart,
  kEntryCost,
  kEntrySpread,
  kParallelLimit,
  kDriftLimit,
  kPendingOrder,
  kDegraded,
  kRiskLimit,
  kMarketFreshness,
};

struct OrderIntent {
  SignalAction action{SignalAction::kNone};
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  OrderSide side{OrderSide::kBuy};
  double price{0.0};
  bool reduce_only{false};
};

struct SignalDecision {
  bool triggered{false};
  SignalAction action{SignalAction::kNone};
  SignalRejectReason reject_reason{SignalRejectReason::kNone};
  std::uint64_t group_id{0};
  double trailing_price{0.0};
  OrderIntent intent;
};

struct SignalMarket {
  QuoteSnapshot lead;
  QuoteSnapshot lag;
  RecorderSnapshot recorder;
};

struct OpenSignalMetrics {
  bool valid{false};
  double lead_move{0.0};
  double lead_side_move{0.0};
  double price_diff{0.0};
  double lag_part_ratio{0.0};
  double target_space{0.0};
  double required_edge{0.0};
  double lag_spread_pct{0.0};
  double lag_spread_buffer{0.0};
};

class SignalEngine {
 public:
  [[nodiscard]] static OpenSignalMetrics BuildOpenLongMetrics(
      const PairConfig& pair, const SignalMarket& market,
      const ThresholdSnapshot& threshold) noexcept {
    return BuildOpenMetrics</*kLong=*/true>(pair, market, threshold);
  }

  [[nodiscard]] static OpenSignalMetrics BuildOpenShortMetrics(
      const PairConfig& pair, const SignalMarket& market,
      const ThresholdSnapshot& threshold) noexcept {
    return BuildOpenMetrics</*kLong=*/false>(pair, market, threshold);
  }

  [[nodiscard]] static SignalDecision TryOpenLong(
      const PairConfig& pair, const SignalMarket& market,
      const ThresholdSnapshot& threshold) noexcept {
    return TryOpen</*kLong=*/true>(pair, market, threshold);
  }

  [[nodiscard]] static SignalDecision TryOpenShort(
      const PairConfig& pair, const SignalMarket& market,
      const ThresholdSnapshot& threshold) noexcept {
    return TryOpen</*kLong=*/false>(pair, market, threshold);
  }

  [[nodiscard]] static SignalDecision OnLeadTick(
      const PairConfig& pair, const ExecutionState& execution,
      const SignalMarket& market, const ThresholdSnapshot& threshold,
      const AlignmentSnapshot& alignment) noexcept {
    for (const ExecutionGroup& group : execution.groups()) {
      if (!group.hold()) {
        continue;
      }
      SignalDecision close =
          group.long_position() ? TryCloseLong(pair, group, market, threshold)
                                : TryCloseShort(pair, group, market, threshold);
      if (close.triggered) {
        return close;
      }
    }
    if (execution.active_group_count() >= execution.capacity()) {
      return Reject(SignalRejectReason::kParallelLimit);
    }
    if (execution.new_entries_paused()) {
      return Reject(SignalRejectReason::kDegraded);
    }
    if (alignment.drift_ready &&
        alignment.drift_deviation > pair.trigger.drift_limit) {
      return Reject(SignalRejectReason::kDriftLimit);
    }
    SignalDecision open_long = TryOpenLong(pair, market, threshold);
    if (open_long.triggered) {
      return open_long;
    }
    return TryOpenShort(pair, market, threshold);
  }

  [[nodiscard]] static SignalDecision OnLagTick(
      const PairConfig& pair, ExecutionState& execution,
      const SignalMarket& market, const ThresholdSnapshot& threshold) noexcept {
    for (ExecutionGroup& group : execution.mutable_groups()) {
      if (!group.hold()) {
        continue;
      }
      SignalDecision stoploss = group.long_position()
                                    ? TryStoplossLong(pair, &group, market)
                                    : TryStoplossShort(pair, &group, market);
      if (stoploss.triggered) {
        return stoploss;
      }
      SignalDecision close =
          group.long_position() ? TryCloseLong(pair, group, market, threshold)
                                : TryCloseShort(pair, group, market, threshold);
      if (close.triggered) {
        return close;
      }
    }
    return {};
  }

 private:
  struct OpenMetricComponents {
    double trigger_price{0.0};
    double lead_move{0.0};
    double lead_side_move{0.0};
    double price_diff{0.0};
    double lag_move{0.0};
    double target_space{0.0};
  };

  [[nodiscard]] static EntryCostBreakdown BuildEntryCostBreakdown(
      const PairConfig& pair, const ThresholdSnapshot& threshold,
      double trigger_price, double lag_spread_buffer,
      double lag_spread_pct) noexcept {
    double normalized_lag_spread_buffer = 0.0;
    if (trigger_price > 0.0) {
      normalized_lag_spread_buffer = lag_spread_buffer / trigger_price;
    }
    return EntryCostBreakdown{
        .fee = pair.lag_taker_fee * 2.0,
        .spread = lag_spread_pct,
        .lag_spread_buffer = normalized_lag_spread_buffer,
        .lead_noise = threshold.lead_noise,
        .lag_noise = threshold.lag_noise,
    };
  }

  template <bool kLong>
  [[nodiscard]] static OpenMetricComponents BuildOpenMetricComponents(
      const SignalMarket& market, const ThresholdSnapshot& threshold,
      double lag_spread_buffer) noexcept {
    const BboExtremaSnapshot& lead_extrema = market.recorder.lead_extrema;
    const BboExtremaSnapshot& lag_extrema = market.recorder.lag_extrema;
    const double trigger_price =
        kLong ? market.lag.ask_price : market.lag.bid_price;
    const double lead_move =
        kLong ? market.lead.bid_price / lead_extrema.bid_min - 1.0
              : market.lead.ask_price / lead_extrema.ask_max - 1.0;
    const double lead_side_move =
        kLong ? market.lead.ask_price / lead_extrema.ask_min - 1.0
              : market.lead.bid_price / lead_extrema.bid_max - 1.0;
    const double price_diff =
        (kLong ? market.lead.bid_price : market.lead.ask_price) /
            trigger_price -
        1.0;
    const double base_price =
        kLong ? std::min(lead_extrema.bid_min, lag_extrema.bid_min)
              : std::max(lead_extrema.ask_max, lag_extrema.ask_max);
    const double lag_move =
        (kLong ? market.lag.bid_price : market.lag.ask_price) / base_price -
        1.0;
    const double target_price =
        kLong
            ? market.lead.bid_price -
                  market.lead.bid_price * threshold.up_exit - lag_spread_buffer
            : market.lead.ask_price -
                  lead_extrema.ask_max * threshold.down_exit +
                  lag_spread_buffer;
    return OpenMetricComponents{
        .trigger_price = trigger_price,
        .lead_move = lead_move,
        .lead_side_move = lead_side_move,
        .price_diff = price_diff,
        .lag_move = lag_move,
        .target_space = target_price / trigger_price - 1.0,
    };
  }

  template <bool kLong>
  [[nodiscard]] static OpenSignalMetrics BuildOpenMetrics(
      const PairConfig& pair, const SignalMarket& market,
      const ThresholdSnapshot& threshold) noexcept {
    if (!market.recorder.lead_extrema.valid ||
        !market.recorder.lag_extrema.valid) {
      return {};
    }
    const double lag_spread_buffer =
        LagSpreadBuffer(market.lag, market.recorder.lag_spread_mean);
    const OpenMetricComponents components =
        BuildOpenMetricComponents<kLong>(market, threshold, lag_spread_buffer);
    const double move_space = components.lead_move - components.lag_move;
    const double lag_part_ratio =
        components.lead_move == 0.0 ? 0.0 : move_space / components.lead_move;
    const double lag_spread_pct = SpreadPct(market.lag);
    const EntryCostBreakdown entry_cost =
        BuildEntryCostBreakdown(pair, threshold, components.trigger_price,
                                lag_spread_buffer, lag_spread_pct);
    return OpenSignalMetrics{
        .valid = true,
        .lead_move = components.lead_move,
        .lead_side_move = components.lead_side_move,
        .price_diff = components.price_diff,
        .lag_part_ratio = lag_part_ratio,
        .target_space = components.target_space,
        .required_edge = entry_cost.RequiredEdgeWithTargetProfit(
            pair.trigger.target_profit_rate),
        .lag_spread_pct = lag_spread_pct,
        .lag_spread_buffer = lag_spread_buffer,
    };
  }

  template <bool kLong>
  [[nodiscard]] static SignalRejectReason OpenRejectReason(
      const PairConfig& pair, const ThresholdSnapshot& threshold,
      const OpenSignalMetrics& metrics) noexcept {
    if (!metrics.valid) {
      return SignalRejectReason::kInvalidState;
    }
    if ((kLong && metrics.price_diff <= 0.0) ||
        (!kLong && metrics.price_diff >= 0.0)) {
      return SignalRejectReason::kPriceDiff;
    }
    if ((kLong && (metrics.lead_move < threshold.up_entry ||
                   metrics.lead_side_move < threshold.up_entry)) ||
        (!kLong && (metrics.lead_move > threshold.down_entry ||
                    metrics.lead_side_move > threshold.down_entry))) {
      return SignalRejectReason::kThreshold;
    }
    if (metrics.lead_move == 0.0 ||
        (kLong ? metrics.lag_part_ratio <= pair.trigger.lag_part
               : metrics.lag_part_ratio < pair.trigger.lag_part)) {
      return SignalRejectReason::kLagPart;
    }
    if ((kLong ? metrics.target_space : -metrics.target_space) <
        metrics.required_edge) {
      return SignalRejectReason::kEntryCost;
    }
    if (metrics.lag_spread_pct > pair.execute.EntrySpreadLimit()) {
      return SignalRejectReason::kEntrySpread;
    }
    return SignalRejectReason::kNone;
  }

  template <bool kLong>
  [[nodiscard]] static SignalDecision TryOpen(
      const PairConfig& pair, const SignalMarket& market,
      const ThresholdSnapshot& threshold) noexcept {
    const OpenSignalMetrics metrics =
        BuildOpenMetrics<kLong>(pair, market, threshold);
    const SignalRejectReason reject =
        OpenRejectReason<kLong>(pair, threshold, metrics);
    if (reject != SignalRejectReason::kNone) {
      return Reject(reject);
    }
    return Trigger(kLong ? SignalAction::kOpenLong : SignalAction::kOpenShort,
                   pair, kLong ? OrderSide::kBuy : OrderSide::kSell,
                   kLong ? market.lag.ask_price : market.lag.bid_price,
                   /*reduce_only=*/false);
  }

  [[nodiscard]] static SignalDecision TryCloseLong(
      const PairConfig& pair, const ExecutionGroup& group,
      const SignalMarket& market, const ThresholdSnapshot& threshold) noexcept {
    if (group.pending_order()) {
      return Reject(SignalRejectReason::kPendingOrder);
    }
    if (!group.CanSubmitNormalClose(pair.execute.close_retry_times)) {
      return {};
    }
    const double lag_diff = market.lead.bid_price / market.lag.bid_price - 1.0;
    if (lag_diff >= threshold.up_exit) {
      return {};
    }
    return AttachGroup(Trigger(SignalAction::kCloseLong, pair, OrderSide::kSell,
                               market.lag.bid_price, /*reduce_only=*/true),
                       group);
  }

  [[nodiscard]] static SignalDecision TryCloseShort(
      const PairConfig& pair, const ExecutionGroup& group,
      const SignalMarket& market, const ThresholdSnapshot& threshold) noexcept {
    if (group.pending_order()) {
      return Reject(SignalRejectReason::kPendingOrder);
    }
    if (!group.CanSubmitNormalClose(pair.execute.close_retry_times)) {
      return {};
    }
    const double lag_diff = market.lead.ask_price / market.lag.ask_price - 1.0;
    if (lag_diff <= threshold.down_exit) {
      return {};
    }
    return AttachGroup(Trigger(SignalAction::kCloseShort, pair, OrderSide::kBuy,
                               market.lag.ask_price, /*reduce_only=*/true),
                       group);
  }

  [[nodiscard]] static SignalDecision TryStoplossLong(
      const PairConfig& pair, ExecutionGroup* group,
      const SignalMarket& market) noexcept {
    if (group->pending_order()) {
      return Reject(SignalRejectReason::kPendingOrder);
    }
    if (market.lag.bid_price > group->trailing_price) {
      group->trailing_price = market.lag.bid_price;
    }
    const double fallback = market.lag.bid_price / group->trailing_price - 1.0;
    if (fallback > -pair.execute.trailing_stop) {
      return {};
    }
    return AttachGroup(
        Trigger(SignalAction::kStoplossLong, pair, OrderSide::kSell,
                market.lag.bid_price * 0.995, /*reduce_only=*/true),
        *group);
  }

  [[nodiscard]] static SignalDecision TryStoplossShort(
      const PairConfig& pair, ExecutionGroup* group,
      const SignalMarket& market) noexcept {
    if (group->pending_order()) {
      return Reject(SignalRejectReason::kPendingOrder);
    }
    if (market.lag.ask_price < group->trailing_price) {
      group->trailing_price = market.lag.ask_price;
    }
    const double fallback =
        -(market.lag.ask_price / group->trailing_price - 1.0);
    if (fallback > -pair.execute.trailing_stop) {
      return {};
    }
    return AttachGroup(
        Trigger(SignalAction::kStoplossShort, pair, OrderSide::kBuy,
                market.lag.ask_price * 1.005, /*reduce_only=*/true),
        *group);
  }

  [[nodiscard]] static SignalDecision Reject(
      SignalRejectReason reason) noexcept {
    return SignalDecision{.reject_reason = reason};
  }

  [[nodiscard]] static SignalDecision Trigger(SignalAction action,
                                              const PairConfig& pair,
                                              OrderSide side, double price,
                                              bool reduce_only) noexcept {
    return SignalDecision{
        .triggered = true,
        .action = action,
        .intent =
            OrderIntent{
                .action = action,
                .exchange = pair.lag_exchange,
                .symbol_id = pair.symbol_id,
                .side = side,
                .price = price,
                .reduce_only = reduce_only,
            },
    };
  }

  [[nodiscard]] static SignalDecision AttachGroup(
      SignalDecision decision, const ExecutionGroup& group) noexcept {
    decision.group_id = group.group_id;
    decision.trailing_price = group.trailing_price;
    return decision;
  }
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_SIGNAL_H_
