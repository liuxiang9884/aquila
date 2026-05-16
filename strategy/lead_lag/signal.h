#ifndef AQUILA_STRATEGY_LEAD_LAG_SIGNAL_H_
#define AQUILA_STRATEGY_LEAD_LAG_SIGNAL_H_

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

class SignalEngine {
 public:
  [[nodiscard]] static SignalDecision TryOpenLong(
      const PairConfig& pair, const SignalMarket& market,
      const ThresholdSnapshot& threshold) noexcept {
    if (!market.recorder.lead_extrema.valid ||
        !market.recorder.lag_extrema.valid) {
      return Reject(SignalRejectReason::kInvalidState);
    }

    const BboExtremaSnapshot& lead_extrema = market.recorder.lead_extrema;
    const BboExtremaSnapshot& lag_extrema = market.recorder.lag_extrema;
    const double lead_move = market.lead.bid_price / lead_extrema.bid_min - 1.0;
    const double ask_diff = market.lead.ask_price / lead_extrema.ask_min - 1.0;
    const double trigger_price = market.lag.ask_price;
    const double price_diff = market.lead.bid_price / trigger_price - 1.0;
    if (price_diff <= 0.0) {
      return Reject(SignalRejectReason::kPriceDiff);
    }
    if (lead_move < threshold.up_entry || ask_diff < threshold.up_entry) {
      return Reject(SignalRejectReason::kThreshold);
    }

    const double base_price =
        std::min(lead_extrema.bid_min, lag_extrema.bid_min);
    const double lag_move = market.lag.bid_price / base_price - 1.0;
    const double move_space = lead_move - lag_move;
    if (lead_move == 0.0 || move_space / lead_move <= pair.trigger.lag_part) {
      return Reject(SignalRejectReason::kLagPart);
    }

    const double lag_spread_buffer =
        LagSpreadBuffer(market.lag, market.recorder.lag_spread_mean);
    const double target_price = market.lead.bid_price -
                                market.lead.bid_price * threshold.up_exit -
                                lag_spread_buffer;
    const double target_space = target_price / trigger_price - 1.0;
    const double lag_spread_pct = SpreadPct(market.lag);
    const EntryCostBreakdown entry_cost = BuildEntryCostBreakdown(
        pair, threshold, trigger_price, lag_spread_buffer, lag_spread_pct);
    const double required_edge = entry_cost.RequiredEdgeWithTargetProfit(
        pair.trigger.target_profit_rate);
    if (target_space < required_edge) {
      return Reject(SignalRejectReason::kEntryCost);
    }
    if (lag_spread_pct > pair.execute.EntrySpreadLimit()) {
      return Reject(SignalRejectReason::kEntrySpread);
    }

    return Trigger(SignalAction::kOpenLong, pair, OrderSide::kBuy,
                   trigger_price, /*reduce_only=*/false);
  }

  [[nodiscard]] static SignalDecision TryOpenShort(
      const PairConfig& pair, const SignalMarket& market,
      const ThresholdSnapshot& threshold) noexcept {
    if (!market.recorder.lead_extrema.valid ||
        !market.recorder.lag_extrema.valid) {
      return Reject(SignalRejectReason::kInvalidState);
    }

    const BboExtremaSnapshot& lead_extrema = market.recorder.lead_extrema;
    const BboExtremaSnapshot& lag_extrema = market.recorder.lag_extrema;
    const double lead_move = market.lead.ask_price / lead_extrema.ask_max - 1.0;
    const double trigger_price = market.lag.bid_price;
    const double price_diff = market.lead.ask_price / trigger_price - 1.0;
    if (price_diff >= 0.0) {
      return Reject(SignalRejectReason::kPriceDiff);
    }

    const double bid_diff = market.lead.bid_price / lead_extrema.bid_max - 1.0;
    if (lead_move > threshold.down_entry || bid_diff > threshold.down_entry) {
      return Reject(SignalRejectReason::kThreshold);
    }

    const double base_price =
        std::max(lead_extrema.ask_max, lag_extrema.ask_max);
    const double lag_move = market.lag.ask_price / base_price - 1.0;
    const double move_space = lead_move - lag_move;
    if (lead_move == 0.0 || move_space / lead_move < pair.trigger.lag_part) {
      return Reject(SignalRejectReason::kLagPart);
    }

    const double lag_spread_buffer =
        LagSpreadBuffer(market.lag, market.recorder.lag_spread_mean);
    const double target_price = market.lead.ask_price -
                                lead_extrema.ask_max * threshold.down_exit +
                                lag_spread_buffer;
    const double target_space = target_price / trigger_price - 1.0;
    const double lag_spread_pct = SpreadPct(market.lag);
    const EntryCostBreakdown entry_cost = BuildEntryCostBreakdown(
        pair, threshold, trigger_price, lag_spread_buffer, lag_spread_pct);
    const double required_edge = entry_cost.RequiredEdgeWithTargetProfit(
        pair.trigger.target_profit_rate);
    if (-target_space < required_edge) {
      return Reject(SignalRejectReason::kEntryCost);
    }
    if (lag_spread_pct > pair.execute.EntrySpreadLimit()) {
      return Reject(SignalRejectReason::kEntrySpread);
    }

    return Trigger(SignalAction::kOpenShort, pair, OrderSide::kSell,
                   trigger_price, /*reduce_only=*/false);
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

  [[nodiscard]] static SignalDecision TryCloseLong(
      const PairConfig& pair, const ExecutionGroup& group,
      const SignalMarket& market, const ThresholdSnapshot& threshold) noexcept {
    if (group.pending_order()) {
      return Reject(SignalRejectReason::kPendingOrder);
    }
    const double lag_diff = market.lead.bid_price / market.lag.bid_price - 1.0;
    if (lag_diff >= threshold.up_exit) {
      return {};
    }
    return AttachGroup(
        Trigger(SignalAction::kCloseLong, pair, OrderSide::kSell,
                market.lag.bid_price, /*reduce_only=*/true),
        group);
  }

  [[nodiscard]] static SignalDecision TryCloseShort(
      const PairConfig& pair, const ExecutionGroup& group,
      const SignalMarket& market, const ThresholdSnapshot& threshold) noexcept {
    if (group.pending_order()) {
      return Reject(SignalRejectReason::kPendingOrder);
    }
    const double lag_diff = market.lead.ask_price / market.lag.ask_price - 1.0;
    if (lag_diff <= threshold.down_exit) {
      return {};
    }
    return AttachGroup(
        Trigger(SignalAction::kCloseShort, pair, OrderSide::kBuy,
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
