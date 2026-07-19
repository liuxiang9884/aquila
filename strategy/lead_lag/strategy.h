#ifndef AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
#define AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "core/market_data/types.h"
#include "core/trading/order_decimal.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_latency.h"
#include "core/trading/order_types.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/alignment.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/drift_guard.h"
#include "strategy/lead_lag/execution_state.h"
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
#include "strategy/lead_lag/market_calc_diagnostics.h"
#endif
#include "strategy/lead_lag/raw_market_state.h"
#include "strategy/lead_lag/recorders.h"
#include "strategy/lead_lag/shadow_evaluation.h"
#include "strategy/lead_lag/signal.h"
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
#include "strategy/lead_lag/strategy_test_hooks.h"
#endif
#include "strategy/lead_lag/threshold.h"

namespace aquila::strategy::leadlag {

enum class PositionAccountingMode : std::uint8_t {
  kExternalOrders,
  kSyntheticSignals,
};

struct StrategyOptions {
  PositionAccountingMode position_accounting{
      PositionAccountingMode::kExternalOrders};
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  bool market_calc_diagnostics_only{false};
#endif
};

enum class PositionDirection : std::uint8_t {
  kNone,
  kLong,
  kShort,
};

struct SignalDiagnostics {
  std::int64_t event_ns{0};
  PairRole role{PairRole::kNone};
  bool price_changed{false};
  QuoteSnapshot lead_raw;
  QuoteSnapshot lead_drifted;
  QuoteSnapshot lag;
  AlignmentSnapshot alignment;
  ThresholdSnapshot threshold;
  RecorderSnapshot recorder;
  std::size_t active_group_count{0};
  std::uint64_t group_id{0};
  PositionDirection position_direction{PositionDirection::kNone};
  double trailing_price{0.0};
};

struct SignalTiming {
  std::int64_t trigger_exchange_ns{0};
  std::int64_t trigger_local_ns{0};
  std::int64_t on_book_ticker_entry_ns{0};
  std::int64_t signal_decision_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lead_local_ns{0};
  std::int64_t lead_book_ticker_id{0};
  std::int64_t lead_freshness_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::int64_t lag_local_ns{0};
  std::int64_t lag_book_ticker_id{0};
  std::int64_t lag_freshness_ns{0};
  std::uint64_t max_lead_freshness_ns{0};
  std::uint64_t max_lag_freshness_ns{0};
};

enum class FreshnessRejectReason : std::uint8_t {
  kNone,
  kStaleLeadQuote,
  kStaleLagQuote,
};

namespace detail {

struct StrategyOrderPositionLogFields {
  std::uint64_t position_id{0};
  std::string_view position_event;
  PositionDirection position_direction{PositionDirection::kNone};
  std::string_view order_role;
  std::uint64_t entry_local_order_id{0};
  std::int64_t order_finished_local_ns{0};
};

[[nodiscard]] inline std::string_view StrategyOrderRoleText(
    SignalAction action, bool reduce_only) noexcept {
  if (reduce_only || action == SignalAction::kCloseLong ||
      action == SignalAction::kCloseShort ||
      action == SignalAction::kStoplossLong ||
      action == SignalAction::kStoplossShort) {
    return "exit";
  }
  if (action == SignalAction::kOpenLong || action == SignalAction::kOpenShort) {
    return "entry";
  }
  return "unknown";
}

[[nodiscard]] inline std::string_view FreshnessRejectReasonText(
    FreshnessRejectReason reason) noexcept {
  switch (reason) {
    case FreshnessRejectReason::kStaleLeadQuote:
      return "stale_lead_quote";
    case FreshnessRejectReason::kStaleLagQuote:
      return "stale_lag_quote";
    case FreshnessRejectReason::kNone:
      return "-";
  }
  return "-";
}

[[nodiscard]] inline std::int64_t StrategyLogRealtimeNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
inline void NotifySubmitStageForTest(
    StrategySubmitStageForTest stage, std::uint64_t parent_id = 0,
    std::uint64_t local_order_id = 0,
    std::uint16_t route_id = core::kAutoGatewayRoute,
    std::uint32_t route_index = 0,
    std::uint32_t submission_route_count = 0) noexcept {
  NotifyStrategySubmitStageObserverForTest(StrategySubmitStageRecordForTest{
      .stage = stage,
      .parent_id = parent_id,
      .local_order_id = local_order_id,
      .route_id = route_id,
      .route_index = route_index,
      .submission_route_count = submission_route_count,
  });
}
#endif

[[nodiscard]] inline std::string_view OrderResponseBookTickerIdPrefix(
    core::OrderResponseKind kind) noexcept {
  switch (kind) {
    case core::OrderResponseKind::kAck:
      return "ack";
    case core::OrderResponseKind::kAccepted:
      return "accepted";
    case core::OrderResponseKind::kRejected:
      return "rejected";
    case core::OrderResponseKind::kUnknownResult:
      return "unknown_result";
    case core::OrderResponseKind::kCancelAccepted:
      return "cancel_accepted";
    case core::OrderResponseKind::kCancelRejected:
      return "cancel_rejected";
  }
  return "response";
}

[[nodiscard]] inline std::string_view OrderFeedbackBookTickerIdPrefix(
    OrderFeedbackKind kind) noexcept {
  switch (kind) {
    case OrderFeedbackKind::kAccepted:
      return "accepted";
    case OrderFeedbackKind::kPartialFilled:
      return "partial_filled";
    case OrderFeedbackKind::kFilled:
      return "filled";
    case OrderFeedbackKind::kCancelled:
      return "cancelled";
    case OrderFeedbackKind::kRejected:
      return "rejected";
    case OrderFeedbackKind::kContinuityLost:
      return "continuity_lost";
  }
  return "feedback";
}

inline void LogStrategySignalTriggered(
    Exchange trigger_exchange, std::int32_t trigger_symbol_id,
    const SignalTiming& timing, std::string_view symbol, std::int32_t symbol_id,
    PairRole role, SignalAction action, OrderSide side, bool reduce_only,
    std::uint64_t position_id, double raw_price) noexcept {
  NOVA_INFO(
      "lead_lag_signal_triggered trigger_exchange={} trigger_symbol_id={} "
      "trigger_exchange_ns={} trigger_local_ns={} "
      "on_book_ticker_entry_ns={} signal_decision_ns={} "
      "lead_exchange_ns={} lead_local_ns={} signal_lead_id={} "
      "lead_freshness_ns={} lag_exchange_ns={} lag_local_ns={} "
      "signal_lag_id={} lag_freshness_ns={} "
      "symbol={} symbol_id={} role={} action={} side={} reduce_only={} "
      "position_id={} raw_price={:.12g}",
      magic_enum::enum_name(trigger_exchange), trigger_symbol_id,
      timing.trigger_exchange_ns, timing.trigger_local_ns,
      timing.on_book_ticker_entry_ns, timing.signal_decision_ns,
      timing.lead_exchange_ns, timing.lead_local_ns, timing.lead_book_ticker_id,
      timing.lead_freshness_ns, timing.lag_exchange_ns, timing.lag_local_ns,
      timing.lag_book_ticker_id, timing.lag_freshness_ns, symbol, symbol_id,
      magic_enum::enum_name(role), magic_enum::enum_name(action),
      magic_enum::enum_name(side), reduce_only ? "true" : "false", position_id,
      raw_price);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategySignalTriggeredLogObserverForTest(
      StrategySignalTriggeredLogRecordForTest{
          .trigger_exchange = trigger_exchange,
          .trigger_symbol_id = trigger_symbol_id,
          .trigger_exchange_ns = timing.trigger_exchange_ns,
          .trigger_local_ns = timing.trigger_local_ns,
          .on_book_ticker_entry_ns = timing.on_book_ticker_entry_ns,
          .signal_decision_ns = timing.signal_decision_ns,
          .lead_exchange_ns = timing.lead_exchange_ns,
          .lead_local_ns = timing.lead_local_ns,
          .signal_lead_id = timing.lead_book_ticker_id,
          .lead_freshness_ns = timing.lead_freshness_ns,
          .lag_exchange_ns = timing.lag_exchange_ns,
          .lag_local_ns = timing.lag_local_ns,
          .signal_lag_id = timing.lag_book_ticker_id,
          .lag_freshness_ns = timing.lag_freshness_ns,
          .symbol = symbol,
          .symbol_id = symbol_id,
          .role = role,
          .action = action,
          .side = side,
          .reduce_only = reduce_only,
          .position_id = position_id,
          .raw_price = raw_price});
#endif
}

inline void LogStrategySignalDecision(
    const SignalTiming& timing, std::string_view symbol, std::int32_t symbol_id,
    SignalAction action, OrderSide side, bool reduce_only, double raw_price,
    double current_order_price, double reference_order_price,
    const TakerBufferConfig& taker_buffer) noexcept {
  constexpr std::string_view kDecision{"sent"};
  NOVA_INFO(
      "lead_lag_signal_decision trigger_exchange_ns={} "
      "trigger_local_ns={} on_book_ticker_entry_ns={} "
      "signal_decision_ns={} lead_exchange_ns={} lead_local_ns={} "
      "signal_lead_id={} lead_freshness_ns={} lag_exchange_ns={} "
      "lag_local_ns={} signal_lag_id={} lag_freshness_ns={} "
      "symbol={} symbol_id={} action={} side={} reduce_only={} "
      "decision={} raw_price={:.12g} "
      "current_order_price={:.12g} reference_order_price={:.12g} "
      "entry_buffer_pct={:.12g} close_buffer_pct={:.12g}",
      timing.trigger_exchange_ns, timing.trigger_local_ns,
      timing.on_book_ticker_entry_ns, timing.signal_decision_ns,
      timing.lead_exchange_ns, timing.lead_local_ns, timing.lead_book_ticker_id,
      timing.lead_freshness_ns, timing.lag_exchange_ns, timing.lag_local_ns,
      timing.lag_book_ticker_id, timing.lag_freshness_ns, symbol, symbol_id,
      magic_enum::enum_name(action), magic_enum::enum_name(side),
      reduce_only ? "true" : "false", kDecision, raw_price, current_order_price,
      reference_order_price, taker_buffer.entry_fixed_pct,
      taker_buffer.normal_close_fixed_pct);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategySignalDecisionLogObserverForTest(
      StrategySignalDecisionLogRecordForTest{
          .trigger_exchange_ns = timing.trigger_exchange_ns,
          .trigger_local_ns = timing.trigger_local_ns,
          .on_book_ticker_entry_ns = timing.on_book_ticker_entry_ns,
          .signal_decision_ns = timing.signal_decision_ns,
          .lead_exchange_ns = timing.lead_exchange_ns,
          .lead_local_ns = timing.lead_local_ns,
          .signal_lead_id = timing.lead_book_ticker_id,
          .lead_freshness_ns = timing.lead_freshness_ns,
          .lag_exchange_ns = timing.lag_exchange_ns,
          .lag_local_ns = timing.lag_local_ns,
          .signal_lag_id = timing.lag_book_ticker_id,
          .lag_freshness_ns = timing.lag_freshness_ns,
          .symbol = symbol,
          .symbol_id = symbol_id,
          .action = action,
          .side = side,
          .reduce_only = reduce_only,
          .decision = kDecision,
          .raw_price = raw_price,
          .current_order_price = current_order_price,
          .reference_order_price = reference_order_price,
          .entry_buffer_pct = taker_buffer.entry_fixed_pct,
          .close_buffer_pct = taker_buffer.normal_close_fixed_pct});
#endif
}

inline void LogStrategyOrderIntent(
    const SignalTiming& timing, std::string_view symbol, std::int32_t symbol_id,
    SignalAction action, OrderSide side, bool reduce_only,
    std::uint64_t position_id, double quantity, double raw_price,
    double order_price, std::uint32_t slippage_ticks, double price_tick,
    double target_open_notional, double estimated_notional,
    std::size_t active_groups, bool freshness_guard_pass = true,
    FreshnessRejectReason freshness_reject_reason =
        FreshnessRejectReason::kNone) noexcept {
  NOVA_INFO(
      "lead_lag_order_intent trigger_exchange_ns={} "
      "trigger_local_ns={} on_book_ticker_entry_ns={} "
      "signal_decision_ns={} lead_exchange_ns={} lead_local_ns={} "
      "signal_lead_id={} lead_freshness_ns={} lag_exchange_ns={} "
      "lag_local_ns={} signal_lag_id={} lag_freshness_ns={} "
      "max_lead_freshness_ns={} "
      "max_lag_freshness_ns={} freshness_guard_pass={} "
      "freshness_reject_reason={} symbol={} symbol_id={} action={} side={} "
      "reduce_only={} position_id={} quantity={:.12g} "
      "price={:.12g} raw_price={:.12g} order_price={:.12g} "
      "slippage_ticks={} price_tick={:.12g} target_open_notional={:.12g} "
      "estimated_notional={:.12g} active_groups={}",
      timing.trigger_exchange_ns, timing.trigger_local_ns,
      timing.on_book_ticker_entry_ns, timing.signal_decision_ns,
      timing.lead_exchange_ns, timing.lead_local_ns, timing.lead_book_ticker_id,
      timing.lead_freshness_ns, timing.lag_exchange_ns, timing.lag_local_ns,
      timing.lag_book_ticker_id, timing.lag_freshness_ns,
      timing.max_lead_freshness_ns, timing.max_lag_freshness_ns,
      freshness_guard_pass ? "true" : "false",
      FreshnessRejectReasonText(freshness_reject_reason), symbol, symbol_id,
      magic_enum::enum_name(action), magic_enum::enum_name(side),
      reduce_only ? "true" : "false", position_id, quantity, order_price,
      raw_price, order_price, slippage_ticks, price_tick, target_open_notional,
      estimated_notional, active_groups);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderIntentLogObserverForTest(
      StrategyOrderIntentLogRecordForTest{
          .trigger_exchange_ns = timing.trigger_exchange_ns,
          .trigger_local_ns = timing.trigger_local_ns,
          .on_book_ticker_entry_ns = timing.on_book_ticker_entry_ns,
          .signal_decision_ns = timing.signal_decision_ns,
          .lead_exchange_ns = timing.lead_exchange_ns,
          .lead_local_ns = timing.lead_local_ns,
          .signal_lead_id = timing.lead_book_ticker_id,
          .lead_freshness_ns = timing.lead_freshness_ns,
          .lag_exchange_ns = timing.lag_exchange_ns,
          .lag_local_ns = timing.lag_local_ns,
          .signal_lag_id = timing.lag_book_ticker_id,
          .lag_freshness_ns = timing.lag_freshness_ns,
          .max_lead_freshness_ns = timing.max_lead_freshness_ns,
          .max_lag_freshness_ns = timing.max_lag_freshness_ns,
          .freshness_guard_pass = freshness_guard_pass,
          .freshness_reject_reason = freshness_reject_reason,
          .symbol = symbol,
          .symbol_id = symbol_id,
          .action = action,
          .side = side,
          .reduce_only = reduce_only,
          .position_id = position_id,
          .quantity = quantity,
          .raw_price = raw_price,
          .order_price = order_price,
          .price = order_price,
          .target_open_notional = target_open_notional,
          .estimated_notional = estimated_notional,
          .active_groups = active_groups});
#endif
}

inline void LogStrategyOrderSubmitted(
    std::uint64_t local_order_id, std::uint64_t parent_id,
    std::uint16_t route_id, Exchange trigger_exchange,
    std::int32_t trigger_symbol_id, const SignalTiming& timing,
    std::string_view symbol, std::int32_t symbol_id, PairRole signal_role,
    std::string_view order_role, SignalAction action, OrderSide side,
    bool reduce_only, const StrategyOrderPositionLogFields& position,
    double quantity, std::string_view quantity_text, double raw_price,
    double order_price, std::string_view price_text,
    std::uint32_t slippage_ticks, double price_tick,
    double target_open_notional, double estimated_notional,
    std::size_t active_groups, core::OrderPlaceStatus place_status,
    bool freshness_guard_pass = true,
    FreshnessRejectReason freshness_reject_reason =
        FreshnessRejectReason::kNone) noexcept {
  NOVA_INFO(
      "lead_lag_order_submitted local_order_id={} parent_id={} route_id={} "
      "trigger_exchange={} "
      "trigger_symbol_id={} trigger_exchange_ns={} "
      "trigger_local_ns={} on_book_ticker_entry_ns={} "
      "signal_decision_ns={} lead_exchange_ns={} lead_local_ns={} "
      "signal_lead_id={} lead_freshness_ns={} lag_exchange_ns={} "
      "lag_local_ns={} signal_lag_id={} lag_freshness_ns={} "
      "max_lead_freshness_ns={} "
      "max_lag_freshness_ns={} freshness_guard_pass={} "
      "freshness_reject_reason={} symbol={} symbol_id={} signal_role={} "
      "order_role={} action={} side={} reduce_only={} "
      "position_id={} position_event={} position_direction={} "
      "entry_local_order_id={} quantity={:.12g} quantity_text={} "
      "raw_price={:.12g} order_price={:.12g} price_text={} "
      "slippage_ticks={} price_tick={:.12g} target_open_notional={:.12g} "
      "estimated_notional={:.12g} active_groups={} place_status={}",
      local_order_id, parent_id, route_id,
      magic_enum::enum_name(trigger_exchange), trigger_symbol_id,
      timing.trigger_exchange_ns, timing.trigger_local_ns,
      timing.on_book_ticker_entry_ns, timing.signal_decision_ns,
      timing.lead_exchange_ns, timing.lead_local_ns, timing.lead_book_ticker_id,
      timing.lead_freshness_ns, timing.lag_exchange_ns, timing.lag_local_ns,
      timing.lag_book_ticker_id, timing.lag_freshness_ns,
      timing.max_lead_freshness_ns, timing.max_lag_freshness_ns,
      freshness_guard_pass ? "true" : "false",
      FreshnessRejectReasonText(freshness_reject_reason), symbol, symbol_id,
      magic_enum::enum_name(signal_role), order_role,
      magic_enum::enum_name(action), magic_enum::enum_name(side),
      reduce_only ? "true" : "false", position.position_id,
      position.position_event,
      magic_enum::enum_name(position.position_direction),
      position.entry_local_order_id, quantity, quantity_text, raw_price,
      order_price, price_text, slippage_ticks, price_tick, target_open_notional,
      estimated_notional, active_groups, magic_enum::enum_name(place_status));
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderSubmittedLogObserverForTest(
      StrategyOrderSubmittedLogRecordForTest{
          .local_order_id = local_order_id,
          .parent_id = parent_id,
          .route_id = route_id,
          .trigger_exchange = trigger_exchange,
          .trigger_symbol_id = trigger_symbol_id,
          .trigger_exchange_ns = timing.trigger_exchange_ns,
          .trigger_local_ns = timing.trigger_local_ns,
          .on_book_ticker_entry_ns = timing.on_book_ticker_entry_ns,
          .signal_decision_ns = timing.signal_decision_ns,
          .lead_exchange_ns = timing.lead_exchange_ns,
          .lead_local_ns = timing.lead_local_ns,
          .signal_lead_id = timing.lead_book_ticker_id,
          .lead_freshness_ns = timing.lead_freshness_ns,
          .lag_exchange_ns = timing.lag_exchange_ns,
          .lag_local_ns = timing.lag_local_ns,
          .signal_lag_id = timing.lag_book_ticker_id,
          .lag_freshness_ns = timing.lag_freshness_ns,
          .max_lead_freshness_ns = timing.max_lead_freshness_ns,
          .max_lag_freshness_ns = timing.max_lag_freshness_ns,
          .freshness_guard_pass = freshness_guard_pass,
          .freshness_reject_reason = freshness_reject_reason,
          .symbol = symbol,
          .symbol_id = symbol_id,
          .signal_role = signal_role,
          .order_role = order_role,
          .action = action,
          .side = side,
          .reduce_only = reduce_only,
          .position_id = position.position_id,
          .position_event = position.position_event,
          .position_direction = position.position_direction,
          .entry_local_order_id = position.entry_local_order_id,
          .quantity = quantity,
          .quantity_text = quantity_text,
          .raw_price = raw_price,
          .order_price = order_price,
          .price_text = price_text,
          .slippage_ticks = slippage_ticks,
          .price_tick = price_tick,
          .target_open_notional = target_open_notional,
          .estimated_notional = estimated_notional,
          .active_groups = active_groups,
          .place_status = place_status});
#endif
}

inline void LogStrategyOrderIntentRejected(
    std::string_view reason, const SignalTiming& timing,
    std::string_view symbol, std::int32_t symbol_id, SignalAction action,
    OrderSide side, bool reduce_only, std::uint64_t position_id,
    double quantity, double raw_price, double order_price,
    std::uint32_t slippage_ticks, double price_tick,
    double target_open_notional, double estimated_notional,
    double gross_before = 0.0, double gross_after = 0.0,
    double max_gross_notional = 0.0, std::uint64_t local_order_id = 0,
    std::string_view place_status = "-", bool freshness_guard_pass = true,
    FreshnessRejectReason freshness_reject_reason =
        FreshnessRejectReason::kNone) noexcept {
  NOVA_WARNING(
      "lead_lag_order_intent_rejected reason={} trigger_exchange_ns={} "
      "trigger_local_ns={} on_book_ticker_entry_ns={} signal_decision_ns={} "
      "lead_exchange_ns={} lead_local_ns={} signal_lead_id={} "
      "lead_freshness_ns={} lag_exchange_ns={} lag_local_ns={} "
      "signal_lag_id={} lag_freshness_ns={} max_lead_freshness_ns={} "
      "max_lag_freshness_ns={} "
      "freshness_guard_pass={} freshness_reject_reason={} symbol={} "
      "symbol_id={} action={} side={} reduce_only={} position_id={} "
      "quantity={:.12g} "
      "price={:.12g} raw_price={:.12g} "
      "order_price={:.12g} slippage_ticks={} price_tick={:.12g} "
      "target_open_notional={:.12g} estimated_notional={:.12g} "
      "gross_before={:.12g} gross_after={:.12g} max_gross_notional={:.12g} "
      "local_order_id={} place_status={}",
      reason, timing.trigger_exchange_ns, timing.trigger_local_ns,
      timing.on_book_ticker_entry_ns, timing.signal_decision_ns,
      timing.lead_exchange_ns, timing.lead_local_ns, timing.lead_book_ticker_id,
      timing.lead_freshness_ns, timing.lag_exchange_ns, timing.lag_local_ns,
      timing.lag_book_ticker_id, timing.lag_freshness_ns,
      timing.max_lead_freshness_ns, timing.max_lag_freshness_ns,
      freshness_guard_pass ? "true" : "false",
      FreshnessRejectReasonText(freshness_reject_reason), symbol, symbol_id,
      magic_enum::enum_name(action), magic_enum::enum_name(side),
      reduce_only ? "true" : "false", position_id, quantity, order_price,
      raw_price, order_price, slippage_ticks, price_tick, target_open_notional,
      estimated_notional, gross_before, gross_after, max_gross_notional,
      local_order_id, place_status);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderIntentRejectedLogObserverForTest(
      StrategyOrderIntentRejectedLogRecordForTest{
          .reason = reason,
          .symbol = symbol,
          .symbol_id = symbol_id,
          .action = action,
          .side = side,
          .reduce_only = reduce_only,
          .quantity = quantity,
          .raw_price = raw_price,
          .order_price = order_price,
          .price_tick = price_tick,
          .estimated_notional = estimated_notional});
#endif
}

inline void LogStrategyOrderResponse(
    const core::OrderResponseEvent& event, const core::StrategyOrder* order,
    const SignalTiming& market_timing) noexcept {
  const core::StrategyOrderTimingSnapshot timing =
      order == nullptr ? core::StrategyOrderTimingSnapshot{}
                       : core::MakeStrategyOrderTimingSnapshot(*order);
  const std::uint64_t parent_id =
      order == nullptr ? event.parent_id : order->parent_id;
  const std::uint16_t route_id =
      order == nullptr ? event.route_id : order->gateway_route_id;
  const std::int64_t exchange_to_local_ns =
      core::LatencyDeltaNs(event.local_receive_ns, event.exchange_ns);
  const std::string_view book_ticker_id_prefix =
      OrderResponseBookTickerIdPrefix(event.kind);
  NOVA_INFO(
      "lead_lag_order_response kind={} local_order_id={} parent_id={} "
      "route_id={} "
      "exchange_order_id={} local_receive_ns={} exchange_ns={} "
      "exchange_to_local_ns={} ack_rtt_ns={} response_rtt_ns={} "
      "lead_exchange_ns={} lag_exchange_ns={} {}_lead_id={} {}_lag_id={}",
      magic_enum::enum_name(event.kind), event.local_order_id, parent_id,
      route_id, event.exchange_order_id, event.local_receive_ns,
      event.exchange_ns, exchange_to_local_ns, timing.ack_rtt_ns,
      timing.response_rtt_ns, market_timing.lead_exchange_ns,
      market_timing.lag_exchange_ns, book_ticker_id_prefix,
      market_timing.lead_book_ticker_id, book_ticker_id_prefix,
      market_timing.lag_book_ticker_id);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderResponseLogObserverForTest(
      StrategyOrderResponseLogRecordForTest{
          .kind = event.kind,
          .local_order_id = event.local_order_id,
          .parent_id = parent_id,
          .route_id = route_id,
          .lead_exchange_ns = market_timing.lead_exchange_ns,
          .lag_exchange_ns = market_timing.lag_exchange_ns,
          .book_ticker_id_prefix = book_ticker_id_prefix,
          .lead_book_ticker_id = market_timing.lead_book_ticker_id,
          .lag_book_ticker_id = market_timing.lag_book_ticker_id,
      });
#endif
}

inline void LogStrategyOrderFeedback(
    const OrderFeedbackEvent& event, const core::StrategyOrder* order,
    const SignalTiming& market_timing) noexcept {
  const std::uint64_t parent_id = order == nullptr ? 0 : order->parent_id;
  const std::uint16_t route_id =
      order == nullptr ? core::kAutoGatewayRoute : order->gateway_route_id;
  const std::string_view book_ticker_id_prefix =
      OrderFeedbackBookTickerIdPrefix(event.kind);
  NOVA_INFO(
      "lead_lag_order_feedback kind={} local_order_id={} "
      "parent_id={} route_id={} exchange_order_id={} "
      "cumulative_filled_quantity={:.12g} left_quantity={:.12g} "
      "cancelled_quantity={:.12g} fill_price={:.12g} role={} "
      "finish_reason={} reject_reason={} exchange_update_ns={} "
      "local_receive_ns={} lead_exchange_ns={} lag_exchange_ns={} "
      "{}_lead_id={} {}_lag_id={}",
      magic_enum::enum_name(event.kind), event.local_order_id, parent_id,
      route_id, event.exchange_order_id, event.cumulative_filled_quantity,
      event.left_quantity, event.cancelled_quantity, event.fill_price,
      magic_enum::enum_name(event.role),
      magic_enum::enum_name(event.finish_reason),
      magic_enum::enum_name(event.reject_reason), event.exchange_update_ns,
      event.local_receive_ns, market_timing.lead_exchange_ns,
      market_timing.lag_exchange_ns, book_ticker_id_prefix,
      market_timing.lead_book_ticker_id, book_ticker_id_prefix,
      market_timing.lag_book_ticker_id);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderFeedbackLogObserverForTest(
      StrategyOrderFeedbackLogRecordForTest{
          .kind = event.kind,
          .local_order_id = event.local_order_id,
          .parent_id = parent_id,
          .route_id = route_id,
          .lead_exchange_ns = market_timing.lead_exchange_ns,
          .lag_exchange_ns = market_timing.lag_exchange_ns,
          .book_ticker_id_prefix = book_ticker_id_prefix,
          .lead_book_ticker_id = market_timing.lead_book_ticker_id,
          .lag_book_ticker_id = market_timing.lag_book_ticker_id,
      });
#endif
}

inline void LogStrategyUnknownResultPause(
    const core::OrderResponseEvent& event,
    const core::StrategyOrder* order) noexcept {
  NOVA_WARNING(
      "lead_lag_unknown_result_pause symbol={} symbol_id={} "
      "local_order_id={} kind={} new_entries_paused=true "
      "needs_reconcile=true reason=order_response_unknown_result",
      order == nullptr ? std::string_view{} : order->symbol,
      order == nullptr ? 0 : order->symbol_id, event.local_order_id,
      magic_enum::enum_name(event.kind));
}

inline void LogStrategyUnknownResultResume(
    const core::StrategyOrder& order,
    std::optional<OrderFeedbackKind> feedback_kind) noexcept {
  NOVA_WARNING(
      "lead_lag_unknown_result_resume symbol={} symbol_id={} "
      "local_order_id={} feedback_kind={} new_entries_paused=false "
      "needs_reconcile=false reason=terminal_feedback_resolved_unknown_result",
      order.symbol, order.symbol_id, order.local_order_id,
      feedback_kind.has_value() ? magic_enum::enum_name(*feedback_kind)
                                : std::string_view{"none"});
}

inline void LogStrategyFeedbackContinuityLost(
    const OrderFeedbackEvent& event) noexcept {
  NOVA_ERROR(
      "lead_lag_feedback_continuity_lost scope={} reason={} sequence={} "
      "local_receive_ns={} new_entries_paused=true needs_reconcile=true",
      magic_enum::enum_name(event.continuity_scope),
      magic_enum::enum_name(event.continuity_reason), event.continuity_sequence,
      event.local_receive_ns);
}

inline void LogStrategyPairDisabledForOrderDecimalPlaces(
    std::string_view symbol, std::int32_t symbol_id,
    std::int32_t price_decimal_places, std::int32_t quantity_decimal_places,
    std::int32_t decimal_place_limit) noexcept {
  NOVA_ERROR(
      "lead_lag_pair_disabled reason=order_decimal_places_out_of_bounds "
      "symbol={} symbol_id={} price_decimal_places={} "
      "quantity_decimal_places={} decimal_place_limit_exclusive={}",
      symbol, symbol_id, price_decimal_places, quantity_decimal_places,
      decimal_place_limit);
}

inline void LogStrategyPairDisabledForOrderMetadata(
    std::string_view symbol, std::int32_t symbol_id, double price_tick,
    double open_notional, double quantity_step,
    double notional_multiplier) noexcept {
  NOVA_ERROR(
      "lead_lag_pair_disabled reason=order_metadata_invalid "
      "symbol={} symbol_id={} price_tick={:.12g} open_notional={:.12g} "
      "quantity_step={:.12g} notional_multiplier={:.12g}",
      symbol, symbol_id, price_tick, open_notional, quantity_step,
      notional_multiplier);
}

inline void LogStrategyOrderFinished(
    const core::StrategyOrder& order, StrategyOrderPositionLogFields position,
    std::size_t active_groups, const SignalTiming& market_timing) noexcept {
  position.order_finished_local_ns = detail::StrategyLogRealtimeNowNs();
  const core::StrategyOrderTimingSnapshot timing =
      core::MakeStrategyOrderTimingSnapshot(order);
  NOVA_INFO(
      "lead_lag_order_finished local_order_id={} parent_id={} route_id={} "
      "symbol_id={} symbol={} "
      "status={} reduce_only={} position_id={} position_direction={} "
      "order_role={} entry_local_order_id={} order_finished_local_ns={} "
      "quantity={:.12g} cumulative_filled_quantity={:.12g} "
      "average_fill_price={:.12g} last_fill_price={:.12g} "
      "exchange_order_id={} active_groups={} request_send_local_ns={} "
      "ack_local_receive_ns={} response_local_receive_ns={} "
      "ack_exchange_ns={} response_exchange_ns={} accepted_exchange_ns={} "
      "finish_exchange_ns={} ack_rtt_ns={} response_rtt_ns={} "
      "ack_exchange_to_local_ns={} response_exchange_to_local_ns={} "
      "exchange_lifecycle_ns={} lead_exchange_ns={} lag_exchange_ns={}",
      order.local_order_id, order.parent_id, order.gateway_route_id,
      order.symbol_id, order.symbol, magic_enum::enum_name(order.status),
      order.reduce_only ? "true" : "false", position.position_id,
      magic_enum::enum_name(position.position_direction), position.order_role,
      position.entry_local_order_id, position.order_finished_local_ns,
      order.quantity, order.cumulative_filled_quantity,
      order.AverageFillPrice(), order.last_fill_price, order.exchange_order_id,
      active_groups, timing.request_send_local_ns, timing.ack_local_receive_ns,
      timing.response_local_receive_ns, timing.ack_exchange_ns,
      timing.response_exchange_ns, timing.accepted_exchange_ns,
      timing.finish_exchange_ns, timing.ack_rtt_ns, timing.response_rtt_ns,
      timing.ack_exchange_to_local_ns, timing.response_exchange_to_local_ns,
      timing.exchange_lifecycle_ns, market_timing.lead_exchange_ns,
      market_timing.lag_exchange_ns);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderFinishedLogObserverForTest(
      StrategyOrderFinishedLogRecordForTest{
          .local_order_id = order.local_order_id,
          .parent_id = order.parent_id,
          .route_id = order.gateway_route_id,
          .position_id = position.position_id,
          .position_direction = position.position_direction,
          .order_role = position.order_role,
          .entry_local_order_id = position.entry_local_order_id,
          .order_finished_local_ns = position.order_finished_local_ns,
          .lead_exchange_ns = market_timing.lead_exchange_ns,
          .lag_exchange_ns = market_timing.lag_exchange_ns});
#endif
}

}  // namespace detail

class Strategy {
 public:
  // Callback references are only valid until the callback returns.
  using TriggeredSignalObserver =
      void (*)(void* context, const BookTicker& trigger_ticker,
               const SignalDecision& decision,
               const SignalDiagnostics& diagnostics) noexcept;

  explicit Strategy(Config config, StrategyOptions options = {})
      : config_(std::move(config)), options_(options) {
    raw_market_state_.Reset(config_);
    InitPairRuntimeStates();
  }

  Strategy(const Strategy&) = delete;
  Strategy& operator=(const Strategy&) = delete;
  Strategy(Strategy&&) noexcept = default;
  Strategy& operator=(Strategy&&) noexcept = default;

#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  [[nodiscard]] std::pair<double, double> CurrentGlobalRiskTotalsForTest()
      const noexcept {
    const GlobalRiskTotals totals = CurrentGlobalRiskTotals();
    return {totals.gross_notional, totals.holding_position};
  }

  [[nodiscard]] std::size_t OrderPriceTextSlotCountForTest() const noexcept {
    return order_price_texts_.size();
  }

  [[nodiscard]] std::uint64_t PrepareOrderPriceTextEraseForTest(
      std::size_t target_index, std::size_t dense_active_count) noexcept {
    if (target_index >= order_price_texts_.size() ||
        dense_active_count > order_price_texts_.size()) {
      return 0;
    }
    std::fill(reserved_open_risk_slot_bits_.begin(),
              reserved_open_risk_slot_bits_.end(), 0);
    for (OrderPriceTextStorage& storage : order_price_texts_) {
      storage = {};
    }
    for (std::size_t index = 0; index < dense_active_count; ++index) {
      OrderPriceTextStorage& storage = order_price_texts_[index];
      storage.local_order_id = index + 1U;
      storage.active = true;
    }
    OrderPriceTextStorage& target = order_price_texts_[target_index];
    target.local_order_id = target_index + 1U;
    target.active = true;
    return target.local_order_id;
  }

  void EraseOrderPriceTextForTest(std::uint64_t local_order_id) noexcept {
    EraseOrderPriceText(local_order_id);
  }

  [[nodiscard]] bool OrderPriceTextSlotActiveForTest(
      std::size_t index) const noexcept {
    return index < order_price_texts_.size() &&
           order_price_texts_[index].active;
  }
#endif

#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  void SetMarketCalcObserver(void* context,
                             MarketCalcObserver observer) noexcept {
    market_calc_observer_context_ = context;
    market_calc_observer_ = observer;
  }
#endif

  void SetTriggeredSignalObserver(void* context,
                                  TriggeredSignalObserver observer) noexcept {
    triggered_signal_observer_context_ = context;
    triggered_signal_observer_ = observer;
  }

  template <typename ContextT>
  void OnBookTicker(const BookTicker& ticker, ContextT& context) noexcept {
    const std::int64_t on_book_ticker_entry_ns =
        detail::StrategyLogRealtimeNowNs();
    last_signal_decision_ = {};
    last_signal_diagnostics_valid_ = false;
    last_market_update_ = MarketUpdate{.symbol_id = ticker.symbol_id};
    PairRoute* route = Route(ticker.symbol_id);
    if (route == nullptr) {
      return;
    }
    PairRole role = PairRole::kNone;
    if (ticker.exchange == route->lead_exchange) {
      role = PairRole::kLead;
    } else if (ticker.exchange == route->lag_exchange) {
      role = PairRole::kLag;
    } else {
      return;
    }

    PairMarketState* market = route->market;
    PairRuntimeState* runtime = route->runtime;
    assert(market != nullptr);
    last_market_update_ = market->Update(role, ticker);
    if (runtime == nullptr) {
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      EmitCurrentMarketCalcRow(ticker, nullptr, *market, last_market_update_);
#endif
      return;
    }

    const std::int64_t now_ns = BookTickerEventTimeNs(ticker);
    if (last_market_update_.both_sides_valid) {
      runtime->alignment.OnPairedRawBbo(now_ns, market->lead.latest_quote,
                                        market->lag.latest_quote);
      runtime->drift_guard.OnPairedRawBbo(now_ns, market->lead.latest_quote,
                                          market->lag.latest_quote);
    }

    const AlignmentPhase previous_phase = runtime->alignment.phase();
    const AlignmentPhase phase = runtime->alignment.UpdatePhase(
        now_ns, market->lead.has_quote, market->lag.has_quote);
    if (phase != AlignmentPhase::kActive) {
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      EmitCurrentMarketCalcRow(ticker, runtime, *market, last_market_update_);
#endif
      return;
    }

    if (previous_phase != AlignmentPhase::kActive) {
      const ActiveSeed seed = market->SelectActiveSeed(
          last_market_update_.role, last_market_update_.price_changed);
      const ActiveTransition transition = runtime->alignment.EnterActive(
          now_ns, seed, last_market_update_.role);
      if (!transition.valid) {
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
        EmitCurrentMarketCalcRow(ticker, runtime, *market, last_market_update_);
#endif
        return;
      }
      runtime->recorder.SeedActive(transition.lead_seed_drifted,
                                   transition.lag_seed);
      runtime->drifted_lead = transition.lead_seed_drifted;
      runtime->has_drifted_lead = true;
    }

    const bool allow_resume_lead =
        runtime->alignment.ConsumeResumeLeadTick(last_market_update_.role);
    if (!last_market_update_.price_changed &&
        previous_phase == AlignmentPhase::kActive && !allow_resume_lead) {
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
      EmitCurrentMarketCalcRow(ticker, runtime, *market, last_market_update_);
#endif
      return;
    }

    switch (last_market_update_.role) {
      case PairRole::kLead:
        OnActiveLeadTick(runtime, *market, ticker, on_book_ticker_entry_ns,
                         context);
        break;
      case PairRole::kLag:
        OnActiveLagTick(runtime, *market, ticker, on_book_ticker_entry_ns,
                        context);
        break;
      case PairRole::kNone:
        break;
    }
  }

  template <typename ContextT>
  void OnOrderResponse(const core::OrderResponseEvent& event,
                       ContextT& context) noexcept {
    const core::StrategyOrder* order_for_log =
        context.FindOrder(event.local_order_id);
    const SignalTiming market_timing = MarketTimingForOrder(order_for_log);
    detail::LogStrategyOrderResponse(event, order_for_log, market_timing);
    if (event.kind == core::OrderResponseKind::kUnknownResult) {
      if (MarkOrderUnknownResultNeedsReconcile(order_for_log)) {
        detail::LogStrategyUnknownResultPause(event, order_for_log);
      }
    }
    ApplyFinishedOrder(event.local_order_id, context, market_timing);
  }

  template <typename ContextT>
  void OnOrderFeedback(const OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    if (event.kind == OrderFeedbackKind::kContinuityLost) {
      detail::LogStrategyFeedbackContinuityLost(event);
      if (recovery_state_ != RecoveryState::kManualIntervention) {
        recovery_state_ = RecoveryState::kDegradedNeedsReconcile;
      }
      for (PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
        if (runtime.initialized) {
          runtime.execution.OnFeedbackContinuityLost(event);
        }
      }
      return;
    }
    const core::StrategyOrder* order_for_log =
        context.FindOrder(event.local_order_id);
    const SignalTiming market_timing = MarketTimingForOrder(order_for_log);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifyStrategyFeedbackStageObserverForTest(
        {.stage = StrategyFeedbackStageForTest::kContextReady});
#endif
    detail::LogStrategyOrderFeedback(event, order_for_log, market_timing);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifyStrategyFeedbackStageObserverForTest(
        {.stage = StrategyFeedbackStageForTest::kFeedbackLogged});
#endif
    ApplyFinishedOrder(event.local_order_id, context, market_timing,
                       event.kind);
  }

  [[nodiscard]] bool ShouldStop() const noexcept {
    return stop_requested_;
  }

  void RequestStop() noexcept {
    stop_requested_ = true;
  }

  [[nodiscard]] const Config& config() const noexcept {
    return config_;
  }

  [[nodiscard]] const RawMarketState& raw_market_state() const noexcept {
    return raw_market_state_;
  }

  [[nodiscard]] const MarketUpdate& last_market_update() const noexcept {
    return last_market_update_;
  }

  [[nodiscard]] bool degraded() const noexcept {
    return RecoveryStatePausesNewEntries(recovery_state());
  }

  [[nodiscard]] RecoveryState recovery_state() const noexcept {
    RecoveryState state = recovery_state_;
    for (const PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
      if (runtime.initialized) {
        state =
            MoreSevereRecoveryState(state, runtime.execution.recovery_state());
      }
    }
    return state;
  }

  [[nodiscard]] bool needs_reconcile() const noexcept {
    if (RecoveryStateNeedsReconcile(recovery_state_)) {
      return true;
    }
    for (const PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
      if (runtime.initialized && runtime.execution.needs_reconcile()) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool manual_intervention() const noexcept {
    return RecoveryStateManualIntervention(recovery_state());
  }

  [[nodiscard]] bool new_entries_paused() const noexcept {
    if (RecoveryStatePausesNewEntries(recovery_state_)) {
      return true;
    }
    for (const PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
      if (runtime.initialized && runtime.execution.new_entries_paused()) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool BeginReconcile() noexcept {
    bool started = false;
    if (recovery_state_ == RecoveryState::kReconciling) {
      started = true;
    } else if (recovery_state_ == RecoveryState::kDegradedNeedsReconcile) {
      recovery_state_ = RecoveryState::kReconciling;
      started = true;
    }
    for (PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
      if (runtime.initialized) {
        started = runtime.execution.BeginReconcile() || started;
      }
    }
    return started;
  }

  [[nodiscard]] bool ApplyRecoveryResult(
      const RecoveryApplyResult& result) noexcept {
    const bool recovered = RecoveryApplySucceeded(result);
    if (!recovered) {
      recovery_state_ = RecoveryState::kManualIntervention;
      for (PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
        if (runtime.initialized) {
          [[maybe_unused]] const bool applied =
              runtime.execution.ApplyRecoveryResult(result);
        }
      }
      return false;
    }

    if (recovery_state_ == RecoveryState::kManualIntervention) {
      return false;
    }

    const bool global_reconciling =
        recovery_state_ == RecoveryState::kReconciling;
    const bool runtime_reconciling = AnyInitializedRuntimeReconciling();
    if (!global_reconciling && !runtime_reconciling) {
      MarkNeedsReconcile();
      for (PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
        if (runtime.initialized && runtime.execution.needs_reconcile()) {
          runtime.execution.MarkNeedsReconcile();
        }
      }
      return false;
    }

    bool all_recovered = true;
    for (PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
      if (!runtime.initialized) {
        continue;
      }
      if (runtime.execution.recovery_state() == RecoveryState::kReconciling) {
        all_recovered =
            runtime.execution.ApplyRecoveryResult(result) && all_recovered;
      } else if (runtime.execution.needs_reconcile()) {
        all_recovered = false;
      }
    }
    recovery_state_ = all_recovered ? RecoveryState::kNormal
                                    : RecoveryState::kDegradedNeedsReconcile;
    return all_recovered;
  }

  [[nodiscard]] const SignalDecision& last_signal_decision() const noexcept {
    return last_signal_decision_;
  }

  [[nodiscard]] const SignalDiagnostics& last_signal_diagnostics()
      const noexcept {
    return last_signal_diagnostics_;
  }

  [[nodiscard]] bool last_signal_diagnostics_valid() const noexcept {
    return last_signal_diagnostics_valid_;
  }

 private:
  static constexpr std::size_t kOrderPriceTextCapacity = 64;
  static constexpr std::int32_t kOrderDecimalPlaceLimit = 12;
  static constexpr std::int32_t kMaxCoreOrderDecimalPlaces =
      ::aquila::core::kMaxOrderDecimalPlaces;

  struct PairRuntimeState {
    bool initialized{false};
    PairConfig pair;
    std::uint64_t max_lead_freshness_ns{0};
    std::uint64_t max_lag_freshness_ns{0};
    AlignmentState alignment;
    DriftGuardState drift_guard;
    RecorderState recorder;
    ThresholdState threshold;
    ExecutionState execution;
    QuoteSnapshot drifted_lead;
    bool has_drifted_lead{false};
    struct OrderDecimalMetadata {
      std::int64_t open_notional_units{0};
      std::int32_t notional_decimal_places{0};
      std::int64_t quantity_step_units{0};
      std::int64_t min_quantity_units{0};
      std::int64_t max_quantity_units{0};
      std::int64_t multiplier_units{1};
      std::int32_t multiplier_decimal_places{0};
    } order_decimal;
  };

  struct PairRoute {
    bool initialized{false};
    Exchange lead_exchange{Exchange::kBinance};
    Exchange lag_exchange{Exchange::kGate};
    PairMarketState* market{nullptr};
    PairRuntimeState* runtime{nullptr};
  };

  struct PreparedOrderPrice {
    bool valid{false};
    std::string_view reject_reason;
    double raw_order_price{0.0};
    double rounded_order_price{0.0};
    double order_price{0.0};
    std::int64_t price_units{0};
    std::uint32_t slippage_ticks{0};
  };

  struct PreparedOrderQuantity {
    bool valid{false};
    double quantity{0.0};
    std::int64_t quantity_units{0};
    ExecutionGroup* close_group{nullptr};
  };

  struct ExternalOrderSubmitResult {
    std::uint64_t local_order_id{0};
    bool accepted{false};
    bool rejected_tracked{false};
  };

  [[nodiscard]] SignalTiming MarketTimingForOrder(
      const core::StrategyOrder* order) const noexcept {
    if (order == nullptr) {
      return {};
    }
    const PairRoute* route = Route(order->symbol_id);
    if (route == nullptr || route->market == nullptr) {
      return {};
    }
    return SignalTiming{
        .lead_exchange_ns = route->market->lead.latest_quote.exchange_ns,
        .lead_local_ns = route->market->lead.latest_quote.local_ns,
        .lead_book_ticker_id = route->market->lead.latest_quote.id,
        .lag_exchange_ns = route->market->lag.latest_quote.exchange_ns,
        .lag_local_ns = route->market->lag.latest_quote.local_ns,
        .lag_book_ticker_id = route->market->lag.latest_quote.id,
    };
  }

  [[nodiscard]] static SignalTiming BuildSignalTiming(
      const BookTicker& trigger_ticker, const PairMarketState& market,
      std::int64_t on_book_ticker_entry_ns, std::int64_t signal_decision_ns,
      const PairRuntimeState& runtime) noexcept {
    return SignalTiming{
        .trigger_exchange_ns = trigger_ticker.exchange_ns,
        .trigger_local_ns = trigger_ticker.local_ns,
        .on_book_ticker_entry_ns = on_book_ticker_entry_ns,
        .signal_decision_ns = signal_decision_ns,
        .lead_exchange_ns = market.lead.latest_quote.exchange_ns,
        .lead_local_ns = market.lead.latest_quote.local_ns,
        .lead_book_ticker_id = market.lead.latest_quote.id,
        .lead_freshness_ns =
            signal_decision_ns - market.lead.latest_quote.exchange_ns,
        .lag_exchange_ns = market.lag.latest_quote.exchange_ns,
        .lag_local_ns = market.lag.latest_quote.local_ns,
        .lag_book_ticker_id = market.lag.latest_quote.id,
        .lag_freshness_ns =
            signal_decision_ns - market.lag.latest_quote.exchange_ns,
        .max_lead_freshness_ns = runtime.max_lead_freshness_ns,
        .max_lag_freshness_ns = runtime.max_lag_freshness_ns,
    };
  }

  [[nodiscard]] static bool OpenAction(SignalAction action) noexcept {
    return action == SignalAction::kOpenLong ||
           action == SignalAction::kOpenShort;
  }

  [[nodiscard]] static bool AppliesOpenFreshnessGuard(
      const SignalDecision& decision) noexcept {
    return OpenAction(decision.action) && !decision.intent.reduce_only;
  }

  [[nodiscard]] static bool SignalDecisionDiagnosticsEnabled(
      const ExecuteConfig& execute) noexcept {
    return execute.taker_buffer.mode != FeatureMode::kOff;
  }

  [[nodiscard]] static FreshnessRejectReason OpenFreshnessRejectReason(
      const SignalTiming& timing) noexcept {
    if (timing.lead_freshness_ns >
        static_cast<std::int64_t>(timing.max_lead_freshness_ns)) {
      return FreshnessRejectReason::kStaleLeadQuote;
    }
    if (timing.lag_freshness_ns >
        static_cast<std::int64_t>(timing.max_lag_freshness_ns)) {
      return FreshnessRejectReason::kStaleLagQuote;
    }
    return FreshnessRejectReason::kNone;
  }

  struct OrderPriceTextStorage {
    std::uint64_t local_order_id{0};
    std::array<char, kOrderPriceTextCapacity> price_text{};
    std::array<char, kOrderPriceTextCapacity> quantity_text{};
    std::size_t price_size{0};
    std::size_t quantity_size{0};
    double reserved_open_quantity{0.0};
    double reserved_open_notional{0.0};
    bool active{false};

    [[nodiscard]] std::string_view price_view() const noexcept {
      return std::string_view(price_text.data(), price_size);
    }

    [[nodiscard]] std::string_view quantity_view() const noexcept {
      return std::string_view(quantity_text.data(), quantity_size);
    }
  };

  struct OpenOrderQuantity {
    double quantity{0.0};
    std::int64_t quantity_units{0};
  };

  void InitPairRuntimeStates() {
    initialized_pair_runtimes_.clear();
    initialized_pair_runtimes_.reserve(config_.pairs.size());
    std::int32_t max_symbol_id = -1;
    for (const PairConfig& pair : config_.pairs) {
      if (pair.symbol_id > max_symbol_id) {
        max_symbol_id = pair.symbol_id;
      }
    }
    if (max_symbol_id < 0) {
      return;
    }

    pair_runtime_by_symbol_id_.clear();
    routes_by_symbol_id_.clear();
    order_price_texts_.clear();
    pair_runtime_by_symbol_id_.resize(
        static_cast<std::size_t>(max_symbol_id + 1));
    routes_by_symbol_id_.resize(static_cast<std::size_t>(max_symbol_id + 1));
    for (const PairConfig& pair : config_.pairs) {
      if (pair.symbol_id < 0) {
        continue;
      }
      PairMarketState* market = raw_market_state_.MutablePair(pair.symbol_id);
      if (market == nullptr) {
        continue;
      }
      routes_by_symbol_id_[static_cast<std::size_t>(pair.symbol_id)] =
          PairRoute{
              .initialized = true,
              .lead_exchange = pair.lead_exchange,
              .lag_exchange = pair.lag_exchange,
              .market = market,
              .runtime = nullptr,
          };
    }
    std::size_t price_text_slot_count = 0;
    for (const PairConfig& pair : config_.pairs) {
      if (!RuntimeConfigReady(pair)) {
        continue;
      }
      if (!OrderDecimalPlacesWithinRuntimeBounds(pair)) {
        detail::LogStrategyPairDisabledForOrderDecimalPlaces(
            pair.symbol, pair.symbol_id,
            pair.lag_instrument.price_decimal_places,
            pair.lag_instrument.quantity_decimal_places,
            kOrderDecimalPlaceLimit);
        continue;
      }
      const std::optional<PairRuntimeState::OrderDecimalMetadata>
          order_decimal = BuildOrderDecimalMetadata(pair);
      if (!order_decimal) {
        detail::LogStrategyPairDisabledForOrderMetadata(
            pair.symbol, pair.symbol_id, pair.lag_instrument.price_tick,
            pair.execute.open_notional, pair.lag_instrument.quantity_step,
            pair.lag_instrument.notional_multiplier);
        continue;
      }
      PairRoute* route = Route(pair.symbol_id);
      if (route == nullptr || route->market == nullptr) {
        continue;
      }
      price_text_slot_count +=
          static_cast<std::size_t>(pair.execute.parallel) *
          static_cast<std::size_t>(kMaxExecutionGroupPendingOrders);
      PairRuntimeState& runtime =
          pair_runtime_by_symbol_id_[static_cast<std::size_t>(pair.symbol_id)];
      runtime.initialized = true;
      runtime.pair = pair;
      runtime.max_lead_freshness_ns =
          static_cast<std::uint64_t>(pair.max_lead_freshness_ms) * 1'000'000ULL;
      runtime.max_lag_freshness_ns =
          static_cast<std::uint64_t>(pair.max_lag_freshness_ms) * 1'000'000ULL;
      runtime.order_decimal = *order_decimal;
      runtime.alignment.Init(AlignmentConfig{
          .drift_period_ns = pair.trigger.drift_period_ns,
          .stats_window_ns = pair.bbo_record.stats_window_ns,
          .drift_warmup_ns = pair.trigger.drift_warmup_ns,
          .drift_min_samples = pair.trigger.drift_min_samples,
          .initial_capacity = pair.capacity.spread_window_capacity,
      });
      runtime.drift_guard.Init(pair.trigger.drift_guard,
                               pair.capacity.drift_guard_window_capacity);
      runtime.recorder.Init(pair);
      runtime.threshold.Init(pair);
      runtime.execution.Init(pair.execute.parallel);
      route->runtime = &runtime;
    }
    for (PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
      if (runtime.initialized) {
        initialized_pair_runtimes_.push_back(&runtime);
      }
    }
    order_price_texts_.resize(price_text_slot_count);
    reserved_open_risk_slot_bits_.assign((price_text_slot_count + 63U) / 64U,
                                         0);
  }

  [[nodiscard]] static bool RuntimeConfigReady(
      const PairConfig& pair) noexcept {
    return pair.symbol_id >= 0 && pair.trigger.drift_period_ns > 0 &&
           pair.bbo_record.window_ns > 0 &&
           pair.bbo_record.stats_window_ns > 0 &&
           pair.trigger.quantile.up_max > pair.trigger.quantile.up_min &&
           pair.trigger.quantile.down_max > pair.trigger.quantile.down_min;
  }

  [[nodiscard]] static std::uint32_t EffectiveOrderSessionFanout(
      const ExecuteConfig& execute) noexcept {
    if (execute.order_session_fanout == 0) {
      return 1;
    }
    return std::min(execute.order_session_fanout, kMaxOrderSessionFanout);
  }

  template <typename ContextT>
  [[nodiscard]] static std::uint32_t ContextOrderSessionFanoutLimit(
      const ContextT& context) noexcept {
    std::uint32_t fanout_limit = 1;
    if constexpr (requires { context.MaxOrderSessionFanout(); }) {
      fanout_limit = context.MaxOrderSessionFanout();
    }
    if (fanout_limit == 0) {
      return 1;
    }
    return std::min(fanout_limit, kMaxOrderSessionFanout);
  }

  template <typename ContextT>
  static void RefreshContextOrderRoutes(ContextT& context) noexcept {
    if constexpr (requires { context.RefreshOrderRoutes(); }) {
      context.RefreshOrderRoutes();
    }
  }

  template <typename ContextT>
  [[nodiscard]] static bool ContextOrderRouteReady(
      const ContextT& context, std::uint16_t route_id) noexcept {
    if constexpr (requires { context.OrderRouteReady(route_id); }) {
      return context.OrderRouteReady(route_id);
    }
    return route_id == 0;
  }

  [[nodiscard]] static bool OrderDecimalPlacesWithinRuntimeBounds(
      const PairConfig& pair) noexcept {
    const InstrumentMetadata& instrument = pair.lag_instrument;
    const std::int32_t price_decimal_places = instrument.price_decimal_places;
    const std::int32_t quantity_decimal_places =
        instrument.quantity_decimal_places;
    return price_decimal_places >= 0 &&
           price_decimal_places < kOrderDecimalPlaceLimit &&
           quantity_decimal_places >= 0 &&
           quantity_decimal_places < kOrderDecimalPlaceLimit &&
           price_decimal_places + quantity_decimal_places <
               kOrderDecimalPlaceLimit;
  }

  [[nodiscard]] PairRuntimeState* MutableRuntime(
      std::int32_t symbol_id) noexcept {
    if (symbol_id < 0 || static_cast<std::size_t>(symbol_id) >=
                             pair_runtime_by_symbol_id_.size()) {
      return nullptr;
    }
    PairRuntimeState& runtime =
        pair_runtime_by_symbol_id_[static_cast<std::size_t>(symbol_id)];
    return runtime.initialized ? &runtime : nullptr;
  }

  [[nodiscard]] PairRoute* Route(std::int32_t symbol_id) noexcept {
    if (symbol_id < 0 ||
        static_cast<std::size_t>(symbol_id) >= routes_by_symbol_id_.size()) {
      return nullptr;
    }
    PairRoute& route =
        routes_by_symbol_id_[static_cast<std::size_t>(symbol_id)];
    return route.initialized ? &route : nullptr;
  }

  [[nodiscard]] const PairRoute* Route(std::int32_t symbol_id) const noexcept {
    if (symbol_id < 0 ||
        static_cast<std::size_t>(symbol_id) >= routes_by_symbol_id_.size()) {
      return nullptr;
    }
    const PairRoute& route =
        routes_by_symbol_id_[static_cast<std::size_t>(symbol_id)];
    return route.initialized ? &route : nullptr;
  }

  [[nodiscard]] bool AllInitializedRuntimesReconciling() const noexcept {
    for (const PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
      if (runtime.initialized &&
          runtime.execution.recovery_state() != RecoveryState::kReconciling) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool AnyInitializedRuntimeReconciling() const noexcept {
    for (const PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
      if (runtime.initialized &&
          runtime.execution.recovery_state() == RecoveryState::kReconciling) {
        return true;
      }
    }
    return false;
  }

  void MarkNeedsReconcile() noexcept {
    if (recovery_state_ != RecoveryState::kManualIntervention) {
      recovery_state_ = RecoveryState::kDegradedNeedsReconcile;
    }
  }

  [[nodiscard]] bool MarkOrderUnknownResultNeedsReconcile(
      const core::StrategyOrder* order) noexcept {
    if (order == nullptr) {
      MarkNeedsReconcile();
      return true;
    }
    PairRuntimeState* runtime = MutableRuntime(order->symbol_id);
    if (runtime == nullptr) {
      MarkNeedsReconcile();
      return true;
    }
    if (runtime->execution.FindPendingOrderByLocalOrderId(
            order->local_order_id) == nullptr) {
      runtime->execution.MarkNeedsReconcile();
      return true;
    }
    return runtime->execution.MarkUnknownResult(order->local_order_id);
  }

#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  [[nodiscard]] bool MarketCalcDiagnosticsOnly() const noexcept {
    return options_.market_calc_diagnostics_only;
  }

  [[nodiscard]] static const QuoteSnapshot* MarketCalcDriftedLead(
      const PairRuntimeState* runtime) noexcept {
    return runtime != nullptr && runtime->has_drifted_lead
               ? &runtime->drifted_lead
               : nullptr;
  }

  [[nodiscard]] static MarketCalcRow BuildMarketCalcBaseRow(
      const BookTicker& ticker, const PairRuntimeState* runtime,
      const MarketUpdate& update, const AlignmentSnapshot& alignment,
      std::uint64_t row_index) noexcept {
    MarketCalcRow row;
    row.row_index = row_index;
    row.role = update.role;
    row.symbol = runtime == nullptr ? std::string_view{} : runtime->pair.symbol;
    row.symbol_id = ticker.symbol_id;
    row.book_ticker_id = ticker.id;
    row.exchange = ticker.exchange;
    row.exchange_ns = ticker.exchange_ns;
    row.local_ns = ticker.local_ns;
    row.event_ns = BookTickerEventTimeNs(ticker);
    row.price_changed = update.price_changed;
    row.both_sides_valid = update.both_sides_valid;
    row.active = alignment.active;
    return row;
  }

  static void FillMarketCalcQuoteFields(
      MarketCalcRow& row, const PairMarketState& market) noexcept {
    if (market.lead.has_quote) {
      row.lead_bid = market.lead.latest_quote.bid_price;
      row.lead_ask = market.lead.latest_quote.ask_price;
    }
    if (market.lag.has_quote) {
      row.lag_bid = market.lag.latest_quote.bid_price;
      row.lag_ask = market.lag.latest_quote.ask_price;
      row.lag_spread =
          market.lag.latest_quote.ask_price - market.lag.latest_quote.bid_price;
      row.lag_spread_pct = SpreadPct(market.lag.latest_quote);
    }
  }

  static void FillMarketCalcStateFields(
      MarketCalcRow& row, const PairMarketState& market,
      const AlignmentSnapshot& alignment, const RecorderSnapshot& recorder,
      const ThresholdSnapshot& threshold,
      const QuoteSnapshot* drifted_lead) noexcept {
    if (alignment.drift_ready) {
      row.drift_mean = alignment.drift_mean;
      row.drift_std_ema = alignment.drift_std_ema;
    }
    if (drifted_lead != nullptr) {
      row.drifted_lead_bid = drifted_lead->bid_price;
      row.drifted_lead_ask = drifted_lead->ask_price;
    }
    if (threshold.initialized) {
      row.up_entry = threshold.up_entry;
      row.down_entry = threshold.down_entry;
      row.up_exit = threshold.up_exit;
      row.down_exit = threshold.down_exit;
    }
    if (alignment.active) {
      row.lead_noise = recorder.lead_noise;
      row.lag_noise = recorder.lag_noise;
      row.lag_spread_mean = recorder.lag_spread_mean;
      if (market.lag.has_quote) {
        row.lag_spread_buffer =
            LagSpreadBuffer(market.lag.latest_quote, recorder.lag_spread_mean);
      }
    }
  }

  static void FillMarketCalcOpenMetrics(
      MarketCalcRow& row, const PairRuntimeState& runtime,
      const PairMarketState& market, const RecorderSnapshot& recorder,
      const ThresholdSnapshot& threshold,
      const QuoteSnapshot& drifted_lead) noexcept {
    if (!market.lag.has_quote || !recorder.lead_extrema.valid ||
        !recorder.lag_extrema.valid) {
      return;
    }
    const SignalMarket signal_market{
        .lead = drifted_lead,
        .lag = market.lag.latest_quote,
        .recorder = recorder,
    };
    const OpenSignalMetrics long_metrics = SignalEngine::BuildOpenLongMetrics(
        runtime.pair, signal_market, threshold);
    if (long_metrics.valid) {
      row.long_lead_move = long_metrics.lead_move;
      row.long_price_diff = long_metrics.price_diff;
      row.long_lag_part_ratio = long_metrics.lag_part_ratio;
      row.long_target_space = long_metrics.target_space;
      row.long_required_edge = long_metrics.required_edge;
      row.lag_spread_pct = long_metrics.lag_spread_pct;
    }
    const OpenSignalMetrics short_metrics = SignalEngine::BuildOpenShortMetrics(
        runtime.pair, signal_market, threshold);
    if (short_metrics.valid) {
      row.short_lead_move = short_metrics.lead_move;
      row.short_price_diff = short_metrics.price_diff;
      row.short_lag_part_ratio = short_metrics.lag_part_ratio;
      row.short_target_space = short_metrics.target_space;
      row.short_required_edge = short_metrics.required_edge;
      row.lag_spread_pct = short_metrics.lag_spread_pct;
    }
  }

  void EmitCurrentMarketCalcRow(const BookTicker& ticker,
                                const PairRuntimeState* runtime,
                                const PairMarketState& market,
                                const MarketUpdate& update) noexcept {
    if (runtime == nullptr) {
      EmitMarketCalcRow(ticker, nullptr, market, update, AlignmentSnapshot{},
                        RecorderSnapshot{}, ThresholdSnapshot{}, nullptr);
      return;
    }
    EmitMarketCalcRow(
        ticker, runtime, market, update, runtime->alignment.Snapshot(),
        runtime->recorder.snapshot(), runtime->threshold.snapshot(),
        MarketCalcDriftedLead(runtime));
  }

  void EmitMarketCalcRow(const BookTicker& ticker,
                         const PairRuntimeState* runtime,
                         const PairMarketState& market,
                         const MarketUpdate& update,
                         const AlignmentSnapshot& alignment,
                         const RecorderSnapshot& recorder,
                         const ThresholdSnapshot& threshold,
                         const QuoteSnapshot* drifted_lead) noexcept {
    if (market_calc_observer_ == nullptr) {
      return;
    }

    MarketCalcRow row = BuildMarketCalcBaseRow(
        ticker, runtime, update, alignment, ++market_calc_row_index_);
    FillMarketCalcQuoteFields(row, market);
    FillMarketCalcStateFields(row, market, alignment, recorder, threshold,
                              drifted_lead);
    if (runtime != nullptr && drifted_lead != nullptr) {
      FillMarketCalcOpenMetrics(row, *runtime, market, recorder, threshold,
                                *drifted_lead);
    }

    market_calc_observer_(market_calc_observer_context_, row);
  }
#endif

  template <typename ContextT>
  void OnActiveLeadTick(PairRuntimeState* runtime,
                        const PairMarketState& market,
                        const BookTicker& trigger_ticker,
                        std::int64_t on_book_ticker_entry_ns,
                        ContextT& context) noexcept {
    QuoteSnapshot drifted_lead =
        runtime->alignment.DriftLead(market.lead.latest_quote);
    runtime->drifted_lead = drifted_lead;
    runtime->has_drifted_lead = true;

    const MoveQuantileRoll roll =
        runtime->recorder.OnLeadActiveTick(drifted_lead);
    const RecorderSnapshot recorder = runtime->recorder.snapshot();
    const AlignmentSnapshot alignment = runtime->alignment.Snapshot();
    const ThresholdSnapshot threshold =
        runtime->threshold.OnMoveRoll(roll, recorder, alignment);
    const SignalMarket signal_market{
        .lead = drifted_lead,
        .lag = market.lag.latest_quote,
        .recorder = recorder,
    };

#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
    EmitMarketCalcRow(trigger_ticker, runtime, market, last_market_update_,
                      alignment, recorder, threshold, &drifted_lead);
    if (MarketCalcDiagnosticsOnly()) {
      return;
    }
#endif

    last_signal_decision_ = SignalEngine::OnLeadTick(
        runtime->pair, runtime->execution, signal_market, threshold, alignment);
    FinalizeActiveSignal(runtime, market, drifted_lead, recorder, alignment,
                         threshold, trigger_ticker, PairRole::kLead,
                         on_book_ticker_entry_ns, context);
  }

  template <typename ContextT>
  void OnActiveLagTick(PairRuntimeState* runtime, const PairMarketState& market,
                       const BookTicker& trigger_ticker,
                       std::int64_t on_book_ticker_entry_ns,
                       ContextT& context) noexcept {
    runtime->recorder.OnLagActiveTick(market.lag.latest_quote);
    if (!runtime->has_drifted_lead) {
      runtime->drifted_lead =
          runtime->alignment.DriftLead(market.lead.latest_quote);
      runtime->has_drifted_lead = true;
    }

    const RecorderSnapshot recorder = runtime->recorder.snapshot();
    const ThresholdSnapshot threshold = runtime->threshold.snapshot();
    const AlignmentSnapshot alignment = runtime->alignment.Snapshot();
    const SignalMarket signal_market{
        .lead = runtime->drifted_lead,
        .lag = market.lag.latest_quote,
        .recorder = recorder,
    };

#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
    EmitMarketCalcRow(trigger_ticker, runtime, market, last_market_update_,
                      alignment, recorder, threshold, &runtime->drifted_lead);
    if (MarketCalcDiagnosticsOnly()) {
      return;
    }
#endif

    last_signal_decision_ = SignalEngine::OnLagTick(
        runtime->pair, runtime->execution, signal_market, threshold);
    FinalizeActiveSignal(runtime, market, runtime->drifted_lead, recorder,
                         alignment, threshold, trigger_ticker, PairRole::kLag,
                         on_book_ticker_entry_ns, context);
  }

  void RecordTriggeredSignal(
      PairRuntimeState* runtime, const PairMarketState& market,
      const QuoteSnapshot& drifted_lead, const RecorderSnapshot& recorder,
      const AlignmentSnapshot& alignment, const ThresholdSnapshot& threshold,
      const BookTicker& trigger_ticker, PairRole signal_role,
      std::int64_t on_book_ticker_entry_ns) noexcept {
    last_signal_timing_ =
        BuildSignalTiming(trigger_ticker, market, on_book_ticker_entry_ns,
                          detail::StrategyLogRealtimeNowNs(), *runtime);
    last_signal_diagnostics_ = BuildSignalDiagnostics(
        *runtime, market, drifted_lead, recorder, alignment, threshold);
    last_signal_diagnostics_valid_ = true;
    detail::LogStrategySignalTriggered(
        trigger_ticker.exchange, trigger_ticker.symbol_id, last_signal_timing_,
        runtime->pair.symbol, runtime->pair.symbol_id, signal_role,
        last_signal_decision_.action, last_signal_decision_.intent.side,
        last_signal_decision_.intent.reduce_only,
        last_signal_decision_.group_id, last_signal_decision_.intent.price);
    if (triggered_signal_observer_ != nullptr) {
      triggered_signal_observer_(triggered_signal_observer_context_,
                                 trigger_ticker, last_signal_decision_,
                                 last_signal_diagnostics_);
    }
  }

  template <typename ContextT>
  void FinalizeActiveSignal(
      PairRuntimeState* runtime, const PairMarketState& market,
      const QuoteSnapshot& drifted_lead, const RecorderSnapshot& recorder,
      const AlignmentSnapshot& alignment, const ThresholdSnapshot& threshold,
      const BookTicker& trigger_ticker, PairRole signal_role,
      std::int64_t on_book_ticker_entry_ns, ContextT& context) noexcept {
    if (!last_signal_decision_.triggered) {
      return;
    }
    RecordTriggeredSignal(runtime, market, drifted_lead, recorder, alignment,
                          threshold, trigger_ticker, signal_role,
                          on_book_ticker_entry_ns);
    if (RejectOpenForParallelLimit(runtime)) {
      return;
    }
    if (RejectOpenForDriftGuard(runtime, market)) {
      return;
    }
    if (SyntheticPositionAccounting()) {
      ApplySyntheticSignal(runtime, last_signal_decision_);
    } else {
      SubmitExternalSignal(runtime, trigger_ticker, signal_role, context);
    }
  }

  [[nodiscard]] bool SyntheticPositionAccounting() const noexcept {
    return options_.position_accounting ==
           PositionAccountingMode::kSyntheticSignals;
  }

  static void ApplySyntheticSignal(PairRuntimeState* runtime,
                                   const SignalDecision& decision) noexcept {
    assert(decision.triggered);
    switch (decision.action) {
      case SignalAction::kOpenLong: {
        [[maybe_unused]] ExecutionGroup* long_group =
            runtime->execution.AddHoldGroup(/*signed_position_quantity=*/1.0,
                                            decision.intent.price);
        break;
      }
      case SignalAction::kOpenShort: {
        [[maybe_unused]] ExecutionGroup* short_group =
            runtime->execution.AddHoldGroup(/*signed_position_quantity=*/-1.0,
                                            decision.intent.price);
        break;
      }
      case SignalAction::kCloseLong:
      case SignalAction::kStoplossLong:
        ClearSyntheticHold(runtime, decision, /*long_position=*/true);
        break;
      case SignalAction::kCloseShort:
      case SignalAction::kStoplossShort:
        ClearSyntheticHold(runtime, decision, /*long_position=*/false);
        break;
      case SignalAction::kNone:
        break;
    }
  }

  static void ClearSyntheticHold(PairRuntimeState* runtime,
                                 const SignalDecision& decision,
                                 bool long_position) noexcept {
    ExecutionGroup* group = runtime->execution.FindGroupById(decision.group_id);
    if (group == nullptr || !group->hold()) {
      return;
    }
    if ((long_position && group->long_position()) ||
        (!long_position && group->short_position())) {
      [[maybe_unused]] const bool cleared =
          runtime->execution.ClearGroupById(decision.group_id);
    }
  }

  [[nodiscard]] static std::string_view LagOrderSymbol(
      const PairConfig& pair, const InstrumentMetadata& instrument) noexcept {
    if (instrument.exchange_symbol.empty()) {
      return std::string_view(pair.symbol);
    }
    return std::string_view(instrument.exchange_symbol);
  }

  void LogOrderIntentRejectedForSignal(
      std::string_view reason, PairRuntimeState* runtime,
      std::string_view symbol, double quantity, double raw_order_price,
      double order_price, std::uint32_t slippage_ticks, double price_tick,
      double estimated_notional, double gross_before = 0.0,
      double gross_after = 0.0, double max_gross_notional = 0.0,
      std::uint64_t local_order_id = 0, std::string_view place_status = "-",
      bool freshness_guard_pass = true,
      FreshnessRejectReason freshness_reject_reason =
          FreshnessRejectReason::kNone) noexcept {
    detail::LogStrategyOrderIntentRejected(
        reason, last_signal_timing_, symbol, runtime->pair.symbol_id,
        last_signal_decision_.action, last_signal_decision_.intent.side,
        last_signal_decision_.intent.reduce_only,
        last_signal_decision_.group_id, quantity, raw_order_price, order_price,
        slippage_ticks, price_tick, runtime->pair.execute.open_notional,
        estimated_notional, gross_before, gross_after, max_gross_notional,
        local_order_id, place_status, freshness_guard_pass,
        freshness_reject_reason);
  }

  [[nodiscard]] PreparedOrderPrice PrepareOrderPrice(
      const PairRuntimeState& runtime, const ExecutionGroup* close_group,
      const InstrumentMetadata& instrument) noexcept {
    PreparedOrderPrice price;
    price.raw_order_price = last_signal_decision_.intent.price;
    price.slippage_ticks = SlippageTicksForAction(
        runtime.pair.execute, last_signal_decision_.action, close_group);
    price.rounded_order_price = SlippedRoundedOrderPrice(
        price.raw_order_price, instrument, last_signal_decision_.intent.side,
        price.slippage_ticks);
    if (price.rounded_order_price <= 0.0) {
      price.reject_reason = "invalid_price";
      return price;
    }
    if (!DecimalUnitsFromValue(price.rounded_order_price,
                               instrument.price_decimal_places,
                               &price.price_units)) {
      price.reject_reason = "order_text_slot_full";
      return price;
    }
    price.order_price =
        DecimalUnitsToValue(price.price_units, instrument.price_decimal_places);
    price.valid = true;
    return price;
  }

  [[nodiscard]] PreparedOrderQuantity PrepareOrderQuantity(
      PairRuntimeState* runtime, const InstrumentMetadata& instrument,
      std::int64_t price_units) noexcept {
    PreparedOrderQuantity result;
    if (last_signal_decision_.intent.reduce_only) {
      result.close_group =
          runtime->execution.FindGroupById(last_signal_decision_.group_id);
      if (result.close_group == nullptr) {
        return result;
      }
      result.quantity = AbsolutePositionQuantity(*result.close_group);
      if (!DecimalUnitsFromValue(result.quantity,
                                 instrument.quantity_decimal_places,
                                 &result.quantity_units)) {
        result.quantity = 0.0;
        return result;
      }
      result.quantity = DecimalUnitsToValue(result.quantity_units,
                                            instrument.quantity_decimal_places);
    } else {
      const OpenOrderQuantity open_quantity =
          CalculateOpenOrderQuantity(*runtime, price_units);
      result.quantity = open_quantity.quantity;
      result.quantity_units = open_quantity.quantity_units;
    }
    result.valid = result.quantity > kQuantityEpsilon;
    return result;
  }

  [[nodiscard]] bool RejectOpenForParallelLimit(
      PairRuntimeState* runtime) noexcept {
    if (!AppliesOpenFreshnessGuard(last_signal_decision_)) {
      return false;
    }
    if (runtime->execution.active_group_count() <
        runtime->execution.capacity()) {
      return false;
    }
    const InstrumentMetadata& instrument = runtime->pair.lag_instrument;
    const std::string_view symbol = LagOrderSymbol(runtime->pair, instrument);
    LogOrderIntentRejectedForSignal("parallel_limit", runtime, symbol, 0.0,
                                    last_signal_decision_.intent.price,
                                    last_signal_decision_.intent.price, 0,
                                    instrument.price_tick, 0.0);
    RejectSignal(SignalRejectReason::kParallelLimit);
    return true;
  }

  [[nodiscard]] bool RejectOpenForFreshness(
      PairRuntimeState* runtime, std::string_view symbol,
      const PreparedOrderPrice& price,
      const InstrumentMetadata& instrument) noexcept {
    if (!AppliesOpenFreshnessGuard(last_signal_decision_)) {
      return false;
    }
    const FreshnessRejectReason reason =
        OpenFreshnessRejectReason(last_signal_timing_);
    if (reason == FreshnessRejectReason::kNone) {
      return false;
    }
    const std::string_view reason_text =
        detail::FreshnessRejectReasonText(reason);
    LogOrderIntentRejectedForSignal(reason_text, runtime, symbol, 0.0,
                                    price.raw_order_price, price.order_price,
                                    price.slippage_ticks, instrument.price_tick,
                                    0.0, 0.0, 0.0, 0.0, 0, "-", false, reason);
    RejectSignal(SignalRejectReason::kMarketFreshness);
    return true;
  }

  [[nodiscard]] bool RejectOpenForDriftGuard(
      PairRuntimeState* runtime, const PairMarketState& market) noexcept {
    if (!AppliesOpenFreshnessGuard(last_signal_decision_)) {
      return false;
    }
    const DriftGuardSnapshot guard = runtime->drift_guard.Evaluate(
        runtime->pair.trigger.drift_guard, market.lead.latest_quote,
        market.lag.latest_quote);
    if (!guard.blocked) {
      return false;
    }
    const InstrumentMetadata& instrument = runtime->pair.lag_instrument;
    const std::string_view symbol = LagOrderSymbol(runtime->pair, instrument);
    LogOrderIntentRejectedForSignal(
        "drift_guard", runtime, symbol, 0.0, last_signal_decision_.intent.price,
        last_signal_decision_.intent.price, 0, instrument.price_tick, 0.0);
    RejectSignal(SignalRejectReason::kDriftGuard);
    return true;
  }

  void LogSignalDecisionForPreparedPrice(
      const PairRuntimeState& runtime, std::string_view symbol,
      const InstrumentMetadata& instrument,
      const PreparedOrderPrice& price) noexcept {
    if (!SignalDecisionDiagnosticsEnabled(runtime.pair.execute)) {
      return;
    }
    const double reference_order_price = ReferenceShadowOrderPrice(
        last_signal_decision_.action, last_signal_decision_.intent.side,
        price.raw_order_price, instrument, runtime.pair.execute.taker_buffer);
    detail::LogStrategySignalDecision(
        last_signal_timing_, symbol, runtime.pair.symbol_id,
        last_signal_decision_.action, last_signal_decision_.intent.side,
        last_signal_decision_.intent.reduce_only, price.raw_order_price,
        price.order_price, reference_order_price,
        runtime.pair.execute.taker_buffer);
  }

  [[nodiscard]] bool RejectOpenForRisk(PairRuntimeState* runtime,
                                       std::string_view symbol,
                                       const PreparedOrderPrice& price,
                                       const InstrumentMetadata& instrument,
                                       double quantity,
                                       double order_notional) noexcept {
    if (last_signal_decision_.intent.reduce_only ||
        GlobalRiskAllowsOpen(quantity, order_notional)) {
      return false;
    }
    const GlobalRiskTotals totals = CurrentGlobalRiskTotals();
    LogOrderIntentRejectedForSignal("risk_limit", runtime, symbol, quantity,
                                    price.raw_order_price, price.order_price,
                                    price.slippage_ticks, instrument.price_tick,
                                    order_notional, totals.gross_notional,
                                    totals.gross_notional + order_notional,
                                    config_.risk.max_gross_notional);
    RejectSignal(SignalRejectReason::kRiskLimit);
    return true;
  }

  void LogPreparedOrderIntent(const PairRuntimeState& runtime,
                              std::string_view symbol,
                              const InstrumentMetadata& instrument,
                              const PreparedOrderPrice& price,
                              const PreparedOrderQuantity& quantity,
                              double order_notional) noexcept {
    detail::LogStrategyOrderIntent(
        last_signal_timing_, symbol, runtime.pair.symbol_id,
        last_signal_decision_.action, last_signal_decision_.intent.side,
        last_signal_decision_.intent.reduce_only,
        last_signal_decision_.group_id, quantity.quantity,
        price.raw_order_price, price.order_price, price.slippage_ticks,
        instrument.price_tick, runtime.pair.execute.open_notional,
        order_notional, runtime.execution.active_group_count());
  }

  void LogOrderSessionFanoutCapped(std::string_view symbol,
                                   const PairRuntimeState& runtime,
                                   std::uint32_t requested_fanout,
                                   std::uint32_t available_fanout) noexcept {
    NOVA_WARNING(
        "lead_lag_order_session_fanout_capped symbol={} symbol_id={} "
        "action={} reduce_only={} requested_fanout={} available_fanout={}",
        symbol, runtime.pair.symbol_id,
        magic_enum::enum_name(last_signal_decision_.action),
        last_signal_decision_.intent.reduce_only ? "true" : "false",
        requested_fanout, available_fanout);
  }

  void LogOrderSessionRouteNotReady(std::string_view symbol,
                                    const PairRuntimeState& runtime,
                                    std::uint16_t route_id) noexcept {
    NOVA_WARNING(
        "lead_lag_order_session_route_not_ready symbol={} symbol_id={} "
        "action={} reduce_only={} route_id={}",
        symbol, runtime.pair.symbol_id,
        magic_enum::enum_name(last_signal_decision_.action),
        last_signal_decision_.intent.reduce_only ? "true" : "false", route_id);
  }

  template <typename ContextT>
  [[nodiscard]] core::OrderPlaceResult PlacePreparedExternalOrder(
      ContextT& context, std::string_view symbol,
      const PreparedOrderQuantity& quantity,
      const OrderPriceTextStorage& order_text_storage, std::uint64_t parent_id,
      std::uint16_t route_id) noexcept {
    return context.PlaceOrder(core::OrderCreateRequest{
        .parent_id = parent_id,
        .exchange = last_signal_decision_.intent.exchange,
        .symbol_id = last_signal_decision_.intent.symbol_id,
        .symbol = symbol,
        .side = last_signal_decision_.intent.side,
        .order_type = OrderType::kLimit,
        .time_in_force = TimeInForce::kImmediateOrCancel,
        .quantity = quantity.quantity,
        .quantity_text = order_text_storage.quantity_view(),
        .price_text = order_text_storage.price_view(),
        .reduce_only = last_signal_decision_.intent.reduce_only,
        .gateway_route_id = route_id,
    });
  }

  void LogExternalOrderPlaceRejected(
      PairRuntimeState* runtime, std::string_view symbol,
      const InstrumentMetadata& instrument, const PreparedOrderPrice& price,
      const PreparedOrderQuantity& quantity, double order_notional,
      const core::OrderPlaceResult& placed) noexcept {
    LogOrderIntentRejectedForSignal(
        "place_local_rejected", runtime, symbol, quantity.quantity,
        price.raw_order_price, price.order_price, price.slippage_ticks,
        instrument.price_tick, order_notional, 0.0, 0.0, 0.0,
        placed.local_order_id, magic_enum::enum_name(placed.status));
  }

  void LogExternalOrderSubmitted(
      PairRuntimeState* runtime, const BookTicker& trigger_ticker,
      PairRole signal_role, const InstrumentMetadata& instrument,
      std::string_view symbol, const PreparedOrderPrice& price,
      const PreparedOrderQuantity& quantity, double order_notional,
      const OrderPriceTextStorage& order_text_storage,
      const core::OrderPlaceResult& placed, std::uint64_t parent_id,
      std::uint16_t route_id) noexcept {
    const ExecutionGroup* submitted_group =
        runtime->execution.FindPendingOrderByLocalOrderId(
            placed.local_order_id);
    const detail::StrategyOrderPositionLogFields position_log =
        BuildSubmittedOrderPositionLogFields(
            submitted_group, last_signal_decision_.action,
            last_signal_decision_.intent.side,
            last_signal_decision_.intent.reduce_only, placed.local_order_id);
    detail::LogStrategyOrderSubmitted(
        placed.local_order_id, parent_id, route_id, trigger_ticker.exchange,
        trigger_ticker.symbol_id, last_signal_timing_, symbol,
        runtime->pair.symbol_id, signal_role,
        detail::StrategyOrderRoleText(last_signal_decision_.action,
                                      last_signal_decision_.intent.reduce_only),
        last_signal_decision_.action, last_signal_decision_.intent.side,
        last_signal_decision_.intent.reduce_only, position_log,
        quantity.quantity, order_text_storage.quantity_view(),
        price.raw_order_price, price.order_price,
        order_text_storage.price_view(), price.slippage_ticks,
        instrument.price_tick, runtime->pair.execute.open_notional,
        order_notional, runtime->execution.active_group_count(), placed.status);
  }

  [[nodiscard]] bool RejectInvalidOrderPrice(
      PairRuntimeState* runtime, std::string_view symbol,
      const InstrumentMetadata& instrument,
      const PreparedOrderPrice& price) noexcept {
    if (price.valid) {
      return false;
    }
    LogOrderIntentRejectedForSignal(
        price.reject_reason, runtime, symbol, 0.0, price.raw_order_price,
        price.rounded_order_price, price.slippage_ticks, instrument.price_tick,
        0.0);
    return true;
  }

  [[nodiscard]] bool RejectInvalidOrderQuantity(
      PairRuntimeState* runtime, std::string_view symbol,
      const InstrumentMetadata& instrument, const PreparedOrderPrice& price,
      const PreparedOrderQuantity& quantity) noexcept {
    if (quantity.valid) {
      return false;
    }
    LogOrderIntentRejectedForSignal("zero_quantity", runtime, symbol,
                                    quantity.quantity, price.raw_order_price,
                                    price.order_price, price.slippage_ticks,
                                    instrument.price_tick, 0.0);
    return true;
  }

  [[nodiscard]] OrderPriceTextStorage* AcquirePreparedOrderText(
      PairRuntimeState* runtime, std::string_view symbol,
      const InstrumentMetadata& instrument, const PreparedOrderPrice& price,
      const PreparedOrderQuantity& quantity, double order_notional) noexcept {
    OrderPriceTextStorage* order_text_storage = AcquireOrderText(
        price.price_units, instrument.price_decimal_places,
        quantity.quantity_units, instrument.quantity_decimal_places);
    if (order_text_storage == nullptr) {
      LogOrderIntentRejectedForSignal("order_text_slot_full", runtime, symbol,
                                      quantity.quantity, price.raw_order_price,
                                      price.order_price, price.slippage_ticks,
                                      instrument.price_tick, order_notional);
    }
    return order_text_storage;
  }

  template <typename ContextT>
  [[nodiscard]] ExternalOrderSubmitResult SubmitPreparedExternalOrder(
      PairRuntimeState* runtime, const BookTicker& trigger_ticker,
      PairRole signal_role, ContextT& context, ExecutionGroup* submit_group,
      const InstrumentMetadata& instrument, std::string_view symbol,
      const PreparedOrderPrice& price, const PreparedOrderQuantity& quantity,
      double order_notional, OrderPriceTextStorage* order_text_storage,
      std::uint64_t parent_id, std::uint16_t route_id) noexcept {
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(
        StrategySubmitStageForTest::kBeforePlaceOrder, parent_id, 0, route_id);
#endif
    const core::OrderPlaceResult placed = PlacePreparedExternalOrder(
        context, symbol, quantity, *order_text_storage, parent_id, route_id);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(
        StrategySubmitStageForTest::kAfterPlaceOrder, parent_id,
        placed.local_order_id, route_id);
#endif
    if (placed.local_order_id == 0) {
      LogExternalOrderPlaceRejected(runtime, symbol, instrument, price,
                                    quantity, order_notional, placed);
      ReleaseOrderPriceText(order_text_storage);
      return {};
    }
    order_text_storage->local_order_id = placed.local_order_id;
    return HandleExternalOrderPlaceResult(
        runtime, trigger_ticker, signal_role, context, submit_group, instrument,
        symbol, price, quantity, order_notional, order_text_storage, placed,
        parent_id, route_id);
  }

  template <typename ContextT>
  [[nodiscard]] ExternalOrderSubmitResult HandleExternalOrderPlaceResult(
      PairRuntimeState* runtime, const BookTicker& trigger_ticker,
      PairRole signal_role, ContextT& context, ExecutionGroup* submit_group,
      const InstrumentMetadata& instrument, std::string_view symbol,
      const PreparedOrderPrice& price, const PreparedOrderQuantity& quantity,
      double order_notional, OrderPriceTextStorage* order_text_storage,
      const core::OrderPlaceResult& placed, std::uint64_t parent_id,
      std::uint16_t route_id) noexcept {
    if (placed.status == core::OrderPlaceStatus::kOk) {
      const bool tracked =
          OnExternalOrderAccepted(runtime, submit_group, placed.local_order_id);
      if (!tracked) {
        return {.local_order_id = placed.local_order_id};
      }
      LogExternalOrderSubmitted(runtime, trigger_ticker, signal_role,
                                instrument, symbol, price, quantity,
                                order_notional, *order_text_storage, placed,
                                parent_id, route_id);
      if (!last_signal_decision_.intent.reduce_only) {
        ReserveOpenRisk(order_text_storage, quantity.quantity, order_notional);
      }
      return {.local_order_id = placed.local_order_id, .accepted = true};
    }

    LogExternalOrderPlaceRejected(runtime, symbol, instrument, price, quantity,
                                  order_notional, placed);
    if (TrackRejectedSubmit(runtime, submit_group, placed.local_order_id)) {
      return {.local_order_id = placed.local_order_id,
              .rejected_tracked = true};
    }
    if (context.RetireFinishedOrder(placed.local_order_id)) {
      EraseOrderPriceText(placed.local_order_id);
    }
    return {.local_order_id = placed.local_order_id};
  }

  [[nodiscard]] std::uint64_t AllocateExecutionParentId() noexcept {
    std::uint64_t parent_id = next_execution_parent_id_++;
    if (next_execution_parent_id_ == 0) {
      next_execution_parent_id_ = 1;
    }
    if (parent_id == 0) {
      parent_id = next_execution_parent_id_++;
    }
    return parent_id;
  }

  [[nodiscard]] std::uint64_t EnsureExecutionParentId(
      ExecutionGroup& group) noexcept {
    if (group.parent_id == 0) {
      group.parent_id = AllocateExecutionParentId();
    }
    return group.parent_id;
  }

  template <typename ContextT>
  void SubmitExternalSignal(PairRuntimeState* runtime,
                            const BookTicker& trigger_ticker,
                            PairRole signal_role, ContextT& context) noexcept {
    assert(runtime != nullptr);
    assert(last_signal_decision_.triggered);
    const InstrumentMetadata& instrument = runtime->pair.lag_instrument;
    const std::string_view symbol = LagOrderSymbol(runtime->pair, instrument);
    ExecutionGroup* close_group = nullptr;
    if (last_signal_decision_.intent.reduce_only) {
      close_group =
          runtime->execution.FindGroupById(last_signal_decision_.group_id);
    }
    const PreparedOrderPrice price =
        PrepareOrderPrice(*runtime, close_group, instrument);
    if (RejectInvalidOrderPrice(runtime, symbol, instrument, price)) {
      return;
    }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(
        StrategySubmitStageForTest::kPricePrepared);
#endif

    LogSignalDecisionForPreparedPrice(*runtime, symbol, instrument, price);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(
        StrategySubmitStageForTest::kSignalDecisionLogged);
#endif

    if (RejectOpenForFreshness(runtime, symbol, price, instrument)) {
      return;
    }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(
        StrategySubmitStageForTest::kFreshnessChecked);
#endif

    const PreparedOrderQuantity quantity =
        PrepareOrderQuantity(runtime, instrument, price.price_units);
    if (RejectInvalidOrderQuantity(runtime, symbol, instrument, price,
                                   quantity)) {
      return;
    }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(
        StrategySubmitStageForTest::kQuantityPrepared);
#endif

    const std::uint32_t requested_order_session_fanout =
        EffectiveOrderSessionFanout(runtime->pair.execute);
    const std::uint32_t available_order_session_fanout =
        ContextOrderSessionFanoutLimit(context);
    if (requested_order_session_fanout > available_order_session_fanout) {
      LogOrderSessionFanoutCapped(symbol, *runtime,
                                  requested_order_session_fanout,
                                  available_order_session_fanout);
    }
    RefreshContextOrderRoutes(context);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(
        StrategySubmitStageForTest::kRoutesRefreshed);
#endif
    const std::uint32_t target_order_session_fanout = std::min(
        requested_order_session_fanout, available_order_session_fanout);
    std::array<std::uint16_t, kMaxOrderSessionFanout> submission_routes{};
    std::uint32_t submission_route_count = 0;
    for (std::uint32_t route = 0;
         route < available_order_session_fanout &&
         submission_route_count < target_order_session_fanout;
         ++route) {
      const std::uint16_t route_id = static_cast<std::uint16_t>(route);
      if (!ContextOrderRouteReady(context, route_id)) {
        LogOrderSessionRouteNotReady(symbol, *runtime, route_id);
        continue;
      }
      submission_routes[submission_route_count++] = route_id;
    }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(
        StrategySubmitStageForTest::kRoutesSelected, 0, 0,
        core::kAutoGatewayRoute, 0, submission_route_count);
#endif

    const double order_notional =
        OrderNotional(quantity.quantity, price.order_price, instrument);
    if (submission_route_count == 0) {
      LogOrderIntentRejectedForSignal("order_route_not_ready", runtime, symbol,
                                      quantity.quantity, price.raw_order_price,
                                      price.order_price, price.slippage_ticks,
                                      instrument.price_tick, order_notional);
      RejectSignal(SignalRejectReason::kOrderRouteNotReady);
      return;
    }
    const double risk_quantity =
        last_signal_decision_.intent.reduce_only
            ? quantity.quantity
            : quantity.quantity * submission_route_count;
    const double risk_order_notional =
        last_signal_decision_.intent.reduce_only
            ? order_notional
            : order_notional * submission_route_count;
    if (RejectOpenForRisk(runtime, symbol, price, instrument, risk_quantity,
                          risk_order_notional)) {
      return;
    }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(StrategySubmitStageForTest::kRiskChecked);
#endif

    LogPreparedOrderIntent(*runtime, symbol, instrument, price, quantity,
                           order_notional);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(
        StrategySubmitStageForTest::kOrderIntentLogged);
#endif

    ExecutionGroup* submit_group = close_group;
    if (!last_signal_decision_.intent.reduce_only) {
      submit_group = runtime->execution.StartOpenGroup();
      if (submit_group == nullptr) {
        LogOrderIntentRejectedForSignal(
            "parallel_limit", runtime, symbol, quantity.quantity,
            price.raw_order_price, price.order_price, price.slippage_ticks,
            instrument.price_tick, order_notional);
        RejectSignal(SignalRejectReason::kParallelLimit);
        return;
      }
      last_signal_decision_.group_id = submit_group->group_id;
    }
    if (submit_group == nullptr) {
      return;
    }
    const std::uint64_t submit_group_id = submit_group->group_id;
    const std::uint64_t parent_id = EnsureExecutionParentId(*submit_group);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(
        StrategySubmitStageForTest::kExecutionGroupReady, parent_id, 0,
        core::kAutoGatewayRoute, 0, submission_route_count);
#endif

    std::uint32_t accepted_orders = 0;
    std::array<std::uint64_t, kMaxOrderSessionFanout>
        rejected_submit_local_order_ids{};
    std::uint8_t rejected_submit_count = 0;
    for (std::uint32_t route_index = 0; route_index < submission_route_count;
         ++route_index) {
      const std::uint16_t route_id = submission_routes[route_index];
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
      detail::NotifySubmitStageForTest(
          StrategySubmitStageForTest::kBeforeAcquireText, parent_id, 0,
          route_id, route_index, submission_route_count);
#endif
      OrderPriceTextStorage* order_text_storage = AcquirePreparedOrderText(
          runtime, symbol, instrument, price, quantity, order_notional);
      if (order_text_storage == nullptr) {
        continue;
      }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
      detail::NotifySubmitStageForTest(
          StrategySubmitStageForTest::kAfterAcquireText, parent_id, 0, route_id,
          route_index, submission_route_count);
#endif
      const ExternalOrderSubmitResult submit_result =
          SubmitPreparedExternalOrder(runtime, trigger_ticker, signal_role,
                                      context, submit_group, instrument, symbol,
                                      price, quantity, order_notional,
                                      order_text_storage, parent_id, route_id);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
      detail::NotifySubmitStageForTest(
          StrategySubmitStageForTest::kAfterSubmitResult, parent_id,
          submit_result.local_order_id, route_id, route_index,
          submission_route_count);
#endif
      if (submit_result.accepted) {
        ++accepted_orders;
      }
      if (submit_result.rejected_tracked &&
          rejected_submit_count < rejected_submit_local_order_ids.size()) {
        rejected_submit_local_order_ids[rejected_submit_count++] =
            submit_result.local_order_id;
      }
    }
    for (std::uint8_t index = 0; index < rejected_submit_count; ++index) {
      ApplyTrackedRejectedSubmit(
          runtime, rejected_submit_local_order_ids[index], context);
    }
    if (!last_signal_decision_.intent.reduce_only && accepted_orders == 0) {
      [[maybe_unused]] const bool cleared =
          runtime->execution.ClearGroupById(submit_group_id);
    }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifySubmitStageForTest(StrategySubmitStageForTest::kSubmitDone,
                                     parent_id, 0, core::kAutoGatewayRoute, 0,
                                     submission_route_count);
#endif
  }

  [[nodiscard]] bool OnExternalOrderAccepted(
      PairRuntimeState* runtime, ExecutionGroup* submit_group,
      std::uint64_t local_order_id) noexcept {
    ExecutionGroup* group = submit_group;
    if (last_signal_decision_.intent.reduce_only) {
      if (group == nullptr ||
          !runtime->execution.StartCloseOrder(
              *group, local_order_id,
              CloseOrderKindForAction(last_signal_decision_.action))) {
        return false;
      }
    } else {
      if (group == nullptr ||
          !runtime->execution.AddOpenOrder(*group, local_order_id)) {
        return false;
      }
      last_signal_decision_.group_id = group->group_id;
    }
    UpdateSubmittedSignalDiagnostics(runtime, group);
    return true;
  }

  template <typename ContextT>
  void ApplyTrackedRejectedSubmit(PairRuntimeState* runtime,
                                  std::uint64_t local_order_id,
                                  ContextT& context) noexcept {
    [[maybe_unused]] const ExecutionApplyResult applied =
        runtime->execution.ApplySubmitRejected(local_order_id);
    if (context.RetireFinishedOrder(local_order_id)) {
      EraseOrderPriceText(local_order_id);
    }
  }

  [[nodiscard]] bool TrackRejectedSubmit(
      PairRuntimeState* runtime, ExecutionGroup* submit_group,
      std::uint64_t local_order_id) noexcept {
    if (!last_signal_decision_.intent.reduce_only || submit_group == nullptr) {
      return false;
    }
    return runtime->execution.StartCloseOrder(
        *submit_group, local_order_id,
        CloseOrderKindForAction(last_signal_decision_.action));
  }

  void UpdateSubmittedSignalDiagnostics(const PairRuntimeState* runtime,
                                        const ExecutionGroup* group) noexcept {
    if (group == nullptr) {
      return;
    }
    last_signal_decision_.group_id = group->group_id;
    if (!last_signal_diagnostics_valid_) {
      return;
    }
    last_signal_diagnostics_.active_group_count =
        runtime->execution.active_group_count();
    last_signal_diagnostics_.group_id = group->group_id;
    last_signal_diagnostics_.trailing_price = group->trailing_price;
  }

  template <typename ContextT>
  void ApplyFinishedOrder(
      std::uint64_t local_order_id, ContextT& context,
      const SignalTiming& market_timing,
      std::optional<OrderFeedbackKind> feedback_kind = std::nullopt) noexcept {
    if (local_order_id == 0) {
      return;
    }
    const core::StrategyOrder* order = context.FindOrder(local_order_id);
    if (order == nullptr || !order->is_finished) {
      return;
    }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifyStrategyFeedbackStageObserverForTest(
        {.stage = StrategyFeedbackStageForTest::kFinishedOrderReady});
#endif
    PairRuntimeState* runtime = MutableRuntime(order->symbol_id);
    if (runtime != nullptr) {
      const detail::StrategyOrderPositionLogFields position_log =
          BuildFinishedOrderPositionLogFields(runtime, *order);
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
      detail::NotifyStrategyFeedbackStageObserverForTest(
          {.stage = StrategyFeedbackStageForTest::kPositionFieldsReady});
#endif
      [[maybe_unused]] const ExecutionApplyResult applied =
          runtime->execution.ApplyTerminalOrder(*order,
                                                runtime->pair.lag_instrument);
      if (runtime->execution.ConsumeUnknownResultAutoRecovered()) {
        detail::LogStrategyUnknownResultResume(*order, feedback_kind);
      }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
      detail::NotifyStrategyFeedbackStageObserverForTest(
          {.stage = StrategyFeedbackStageForTest::kExecutionApplied});
#endif
      detail::LogStrategyOrderFinished(*order, position_log,
                                       runtime->execution.active_group_count(),
                                       market_timing);
    } else {
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
      detail::NotifyStrategyFeedbackStageObserverForTest(
          {.stage = StrategyFeedbackStageForTest::kPositionFieldsReady});
      detail::NotifyStrategyFeedbackStageObserverForTest(
          {.stage = StrategyFeedbackStageForTest::kExecutionApplied});
#endif
      detail::LogStrategyOrderFinished(
          *order, BuildFinishedOrderPositionLogFields(nullptr, *order), 0,
          market_timing);
    }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifyStrategyFeedbackStageObserverForTest(
        {.stage = StrategyFeedbackStageForTest::kFinishedLogged});
#endif
    if (context.RetireFinishedOrder(local_order_id)) {
      EraseOrderPriceText(local_order_id);
    }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
    detail::NotifyStrategyFeedbackStageObserverForTest(
        {.stage = StrategyFeedbackStageForTest::kRetired});
#endif
  }

  void EraseOrderPriceText(std::uint64_t local_order_id) noexcept {
    for (OrderPriceTextStorage& storage : order_price_texts_) {
      if (storage.active && storage.local_order_id == local_order_id) {
        ReleaseOrderPriceText(&storage);
        return;
      }
    }
  }

  void RejectSignal(SignalRejectReason reason) noexcept {
    last_signal_decision_ = SignalDecision{.reject_reason = reason};
    last_signal_diagnostics_valid_ = false;
  }

  [[nodiscard]] static double AbsolutePositionQuantity(
      const ExecutionGroup& group) noexcept {
    if (!std::isfinite(group.signed_position_quantity)) {
      return 0.0;
    }
    return std::abs(group.signed_position_quantity);
  }

  [[nodiscard]] static std::optional<PairRuntimeState::OrderDecimalMetadata>
  BuildOrderDecimalMetadata(const PairConfig& pair) noexcept {
    PairRuntimeState::OrderDecimalMetadata metadata;
    const InstrumentMetadata& instrument = pair.lag_instrument;
    const std::int32_t price_decimal_places = instrument.price_decimal_places;
    const std::int32_t quantity_decimal_places =
        instrument.quantity_decimal_places;
    const std::int32_t notional_decimal_places =
        price_decimal_places + quantity_decimal_places;
    if (!std::isfinite(instrument.price_tick) || instrument.price_tick <= 0.0 ||
        !std::isfinite(instrument.notional_multiplier) ||
        instrument.notional_multiplier <= 0.0 ||
        !std::isfinite(instrument.quantity_step) ||
        instrument.quantity_step <= 0.0 ||
        !std::isfinite(pair.execute.open_notional) ||
        pair.execute.open_notional <= 0.0) {
      return std::nullopt;
    }

    if (!DecimalUnitsFromValue(pair.execute.open_notional,
                               notional_decimal_places,
                               &metadata.open_notional_units) ||
        !DecimalUnitsFromValue(instrument.quantity_step,
                               quantity_decimal_places,
                               &metadata.quantity_step_units) ||
        !DecimalUnitsFromValueAutoPlaces(instrument.notional_multiplier,
                                         &metadata.multiplier_units,
                                         &metadata.multiplier_decimal_places)) {
      return std::nullopt;
    }
    if (instrument.min_quantity > 0.0 &&
        !DecimalUnitsFromValue(instrument.min_quantity, quantity_decimal_places,
                               &metadata.min_quantity_units)) {
      return std::nullopt;
    }
    if (instrument.max_quantity > 0.0 &&
        !DecimalUnitsFromValue(instrument.max_quantity, quantity_decimal_places,
                               &metadata.max_quantity_units)) {
      return std::nullopt;
    }
    metadata.notional_decimal_places = notional_decimal_places;
    return metadata;
  }

  [[nodiscard]] static OpenOrderQuantity CalculateOpenOrderQuantity(
      const PairRuntimeState& runtime, std::int64_t price_units) noexcept {
    const InstrumentMetadata& instrument = runtime.pair.lag_instrument;
    const PairRuntimeState::OrderDecimalMetadata& metadata =
        runtime.order_decimal;
    assert(price_units > 0);

    const ::aquila::core::OpenQuantityUnitsResult result =
        ::aquila::core::CalculateOpenQuantityUnits(
            ::aquila::core::OpenQuantityUnitsInput{
                .notional_units = metadata.open_notional_units,
                .notional_decimal_places = metadata.notional_decimal_places,
                .price_units = price_units,
                .price_decimal_places = instrument.price_decimal_places,
                .multiplier_units = metadata.multiplier_units,
                .multiplier_decimal_places = metadata.multiplier_decimal_places,
                .quantity_decimal_places = instrument.quantity_decimal_places,
                .quantity_step_units = metadata.quantity_step_units,
                .min_quantity_units = metadata.min_quantity_units,
                .max_quantity_units = metadata.max_quantity_units,
            });
    if (result.status != ::aquila::core::OpenQuantityUnitsStatus::kOk) {
      return {};
    }
    return {.quantity = DecimalUnitsToValue(result.quantity_units,
                                            instrument.quantity_decimal_places),
            .quantity_units = result.quantity_units};
  }

  struct GlobalRiskTotals {
    double gross_notional{0.0};
    double holding_position{0.0};
  };

  [[nodiscard]] GlobalRiskTotals CurrentGlobalRiskTotals() const noexcept {
    GlobalRiskTotals totals;
    for (const PairRuntimeState* runtime_ptr : initialized_pair_runtimes_) {
      const PairRuntimeState& runtime = *runtime_ptr;
      for (const ExecutionGroup& group : runtime.execution.groups()) {
        const double quantity = AbsolutePositionQuantity(group);
        if (quantity <= kQuantityEpsilon) {
          continue;
        }
        totals.gross_notional += OrderNotional(quantity, group.trailing_price,
                                               runtime.pair.lag_instrument);
        totals.holding_position += quantity;
      }
    }
    for (std::size_t word_index = 0;
         word_index < reserved_open_risk_slot_bits_.size(); ++word_index) {
      std::uint64_t active_slots = reserved_open_risk_slot_bits_[word_index];
      while (active_slots != 0) {
        const std::size_t bit_index =
            static_cast<std::size_t>(std::countr_zero(active_slots));
        const std::size_t storage_index = word_index * 64U + bit_index;
        const OrderPriceTextStorage& storage =
            order_price_texts_[storage_index];
        if (storage.active &&
            storage.reserved_open_quantity > kQuantityEpsilon) {
          totals.gross_notional += storage.reserved_open_notional;
          totals.holding_position += storage.reserved_open_quantity;
        }
        active_slots &= active_slots - 1U;
      }
    }
    return totals;
  }

  [[nodiscard]] bool GlobalRiskAllowsOpen(double quantity,
                                          double notional) const noexcept {
    if (quantity <= kQuantityEpsilon || !std::isfinite(notional) ||
        notional <= 0.0) {
      return false;
    }
    if (!config_.risk.GrossNotionalLimitEnabled() &&
        !config_.risk.HoldingPositionLimitEnabled()) {
      return true;
    }

    const GlobalRiskTotals totals = CurrentGlobalRiskTotals();
    if (config_.risk.GrossNotionalLimitEnabled() &&
        totals.gross_notional + notional >
            config_.risk.max_gross_notional + kQuantityEpsilon) {
      return false;
    }
    if (config_.risk.HoldingPositionLimitEnabled()) {
      if (totals.holding_position >=
          config_.risk.max_holding_position - kQuantityEpsilon) {
        return false;
      }
      if (quantity > config_.risk.max_holding_position -
                         totals.holding_position + kQuantityEpsilon) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static double OrderNotional(
      double quantity, double price,
      const InstrumentMetadata& instrument) noexcept {
    assert(std::isfinite(instrument.notional_multiplier));
    assert(instrument.notional_multiplier > 0.0);
    if (quantity <= kQuantityEpsilon || !std::isfinite(price) || price <= 0.0) {
      return 0.0;
    }
    return quantity * price * instrument.notional_multiplier;
  }

  [[nodiscard]] static double RoundedOrderPrice(
      double intent_price, const InstrumentMetadata& instrument,
      OrderSide side) noexcept {
    assert(std::isfinite(instrument.price_tick));
    assert(instrument.price_tick > 0.0);
    if (!std::isfinite(intent_price) || intent_price <= 0.0) {
      return 0.0;
    }
    const double scaled = intent_price / instrument.price_tick;
    if (!std::isfinite(scaled)) {
      return 0.0;
    }
    const double units = side == OrderSide::kBuy
                             ? std::ceil(scaled - kPriceEpsilon)
                             : std::floor(scaled + kPriceEpsilon);
    const double rounded = units * instrument.price_tick;
    return std::isfinite(rounded) && rounded > 0.0 ? rounded : 0.0;
  }

  [[nodiscard]] static double SlippedRoundedOrderPrice(
      double raw_price, const InstrumentMetadata& instrument, OrderSide side,
      std::uint32_t slippage_ticks) noexcept {
    assert(std::isfinite(instrument.price_tick));
    assert(instrument.price_tick > 0.0);
    if (!std::isfinite(raw_price) || raw_price <= 0.0) {
      return 0.0;
    }
    const double slippage =
        static_cast<double>(slippage_ticks) * instrument.price_tick;
    const double adjusted_price =
        side == OrderSide::kBuy ? raw_price + slippage : raw_price - slippage;
    return RoundedOrderPrice(adjusted_price, instrument, side);
  }

  [[nodiscard]] static std::uint32_t SlippageTicksForAction(
      const ExecuteConfig& execute, SignalAction action,
      const ExecutionGroup* close_group) noexcept {
    switch (action) {
      case SignalAction::kOpenLong:
      case SignalAction::kOpenShort:
        return execute.open_slippage_ticks;
      case SignalAction::kCloseLong:
      case SignalAction::kCloseShort:
        return NormalCloseSlippageTicks(execute, close_group);
      case SignalAction::kStoplossLong:
      case SignalAction::kStoplossShort:
        return execute.stoploss_slippage_ticks;
      case SignalAction::kNone:
        return 0;
    }
    return 0;
  }

  [[nodiscard]] static CloseOrderKind CloseOrderKindForAction(
      SignalAction action) noexcept {
    switch (action) {
      case SignalAction::kCloseLong:
      case SignalAction::kCloseShort:
        return CloseOrderKind::kNormal;
      case SignalAction::kStoplossLong:
      case SignalAction::kStoplossShort:
        return CloseOrderKind::kStoploss;
      case SignalAction::kOpenLong:
      case SignalAction::kOpenShort:
      case SignalAction::kNone:
        return CloseOrderKind::kNone;
    }
    return CloseOrderKind::kNone;
  }

  [[nodiscard]] static std::uint32_t NormalCloseSlippageTicks(
      const ExecuteConfig& execute,
      const ExecutionGroup* close_group) noexcept {
    if (close_group == nullptr || close_group->normal_close_retry_count == 0) {
      return execute.close_slippage_ticks;
    }
    const std::uint64_t retry_ticks =
        static_cast<std::uint64_t>(close_group->normal_close_retry_count) *
        static_cast<std::uint64_t>(execute.close_retry_slippage_step_ticks);
    const std::uint64_t total =
        static_cast<std::uint64_t>(execute.close_slippage_ticks) + retry_ticks;
    return total > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(total);
  }

  [[nodiscard]] static bool DecimalUnitsFromValue(
      double value, std::int32_t decimal_places, std::int64_t* units) noexcept {
    const double scaled =
        value * static_cast<double>(::aquila::core::Pow10Int64(decimal_places));
    if (!std::isfinite(scaled) || scaled <= 0.0 ||
        scaled >
            static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    const std::int64_t scaled_units =
        static_cast<std::int64_t>(std::llround(scaled));
    if (scaled_units <= 0) {
      return false;
    }
    *units = scaled_units;
    return true;
  }

  [[nodiscard]] static bool DecimalUnitsFromValueAutoPlaces(
      double value, std::int64_t* units,
      std::int32_t* decimal_places) noexcept {
    for (std::int32_t places = 0; places <= kMaxCoreOrderDecimalPlaces;
         ++places) {
      std::int64_t candidate_units = 0;
      if (!DecimalUnitsFromValue(value, places, &candidate_units)) {
        continue;
      }
      const double restored = DecimalUnitsToValue(candidate_units, places);
      const double tolerance =
          std::max(kQuantityEpsilon, std::abs(value) * kQuantityEpsilon);
      if (std::abs(restored - value) <= tolerance) {
        *units = candidate_units;
        *decimal_places = places;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] static double DecimalUnitsToValue(
      std::int64_t units, std::int32_t decimal_places) noexcept {
    return static_cast<double>(units) /
           static_cast<double>(::aquila::core::Pow10Int64(decimal_places));
  }

  [[nodiscard]] OrderPriceTextStorage* AcquireOrderText(
      std::int64_t price_units, std::int32_t price_decimal_places,
      std::int64_t quantity_units,
      std::int32_t quantity_decimal_places) noexcept {
    assert(price_units > 0);
    assert(quantity_units > 0);
    for (OrderPriceTextStorage& storage : order_price_texts_) {
      if (storage.active) {
        continue;
      }
      storage.price_size =
          ::aquila::core::FormatDecimalUnits(price_units, price_decimal_places,
                                             storage.price_text)
              .size();
      storage.quantity_size =
          ::aquila::core::FormatDecimalUnits(
              quantity_units, quantity_decimal_places, storage.quantity_text)
              .size();
      storage.local_order_id = 0;
      storage.reserved_open_quantity = 0.0;
      storage.reserved_open_notional = 0.0;
      storage.active = true;
      return &storage;
    }
    return nullptr;
  }

  void ReserveOpenRisk(OrderPriceTextStorage* storage, double quantity,
                       double notional) noexcept {
    if (storage == nullptr || quantity <= kQuantityEpsilon ||
        !std::isfinite(notional) || notional <= 0.0) {
      return;
    }
    storage->reserved_open_quantity = quantity;
    storage->reserved_open_notional = notional;
    SetReservedOpenRiskSlot(storage, true);
  }

  void ReleaseOrderPriceText(OrderPriceTextStorage* storage) noexcept {
    if (storage == nullptr) {
      return;
    }
    SetReservedOpenRiskSlot(storage, false);
    storage->local_order_id = 0;
    storage->price_size = 0;
    storage->quantity_size = 0;
    storage->reserved_open_quantity = 0.0;
    storage->reserved_open_notional = 0.0;
    storage->active = false;
  }

  void SetReservedOpenRiskSlot(const OrderPriceTextStorage* storage,
                               bool reserved) noexcept {
    assert(storage >= order_price_texts_.data());
    assert(storage < order_price_texts_.data() + order_price_texts_.size());
    const std::size_t storage_index =
        static_cast<std::size_t>(storage - order_price_texts_.data());
    const std::size_t word_index = storage_index / 64U;
    const std::uint64_t mask = std::uint64_t{1}
                               << static_cast<unsigned>(storage_index % 64U);
    if (reserved) {
      reserved_open_risk_slot_bits_[word_index] |= mask;
    } else {
      reserved_open_risk_slot_bits_[word_index] &= ~mask;
    }
  }

  [[nodiscard]] SignalDiagnostics BuildSignalDiagnostics(
      const PairRuntimeState& runtime, const PairMarketState& market,
      const QuoteSnapshot& lead_drifted, const RecorderSnapshot& recorder,
      const AlignmentSnapshot& alignment,
      const ThresholdSnapshot& threshold) const noexcept {
    return SignalDiagnostics{
        .event_ns = market.last_event_ns,
        .role = last_market_update_.role,
        .price_changed = last_market_update_.price_changed,
        .lead_raw = market.lead.latest_quote,
        .lead_drifted = lead_drifted,
        .lag = market.lag.latest_quote,
        .alignment = alignment,
        .threshold = threshold,
        .recorder = recorder,
        .active_group_count = runtime.execution.active_group_count(),
        .group_id = last_signal_decision_.group_id,
        .position_direction =
            PositionDirectionForAction(last_signal_decision_.action),
        .trailing_price = last_signal_decision_.trailing_price,
    };
  }

  [[nodiscard]] static PositionDirection PositionDirectionForAction(
      SignalAction action) noexcept {
    switch (action) {
      case SignalAction::kOpenLong:
      case SignalAction::kCloseLong:
      case SignalAction::kStoplossLong:
        return PositionDirection::kLong;
      case SignalAction::kOpenShort:
      case SignalAction::kCloseShort:
      case SignalAction::kStoplossShort:
        return PositionDirection::kShort;
      case SignalAction::kNone:
        return PositionDirection::kNone;
    }
    return PositionDirection::kNone;
  }

  [[nodiscard]] static PositionDirection PositionDirectionForOrderSide(
      OrderSide side, bool reduce_only) noexcept {
    if (side == OrderSide::kBuy) {
      return reduce_only ? PositionDirection::kShort : PositionDirection::kLong;
    }
    return reduce_only ? PositionDirection::kLong : PositionDirection::kShort;
  }

  [[nodiscard]] static PositionDirection PositionDirectionForOrderGroup(
      const ExecutionGroup* group, SignalAction action, OrderSide side,
      bool reduce_only) noexcept {
    if (group != nullptr) {
      if (group->long_position()) {
        return PositionDirection::kLong;
      }
      if (group->short_position()) {
        return PositionDirection::kShort;
      }
    }
    const PositionDirection action_direction =
        PositionDirectionForAction(action);
    if (action_direction != PositionDirection::kNone) {
      return action_direction;
    }
    return PositionDirectionForOrderSide(side, reduce_only);
  }

  [[nodiscard]] static detail::StrategyOrderPositionLogFields
  BuildSubmittedOrderPositionLogFields(const ExecutionGroup* group,
                                       SignalAction action, OrderSide side,
                                       bool reduce_only,
                                       std::uint64_t local_order_id) noexcept {
    const std::uint64_t entry_local_order_id =
        group == nullptr ? (reduce_only ? 0 : local_order_id)
                         : (group->entry_local_order_id == 0
                                ? (reduce_only ? 0 : local_order_id)
                                : group->entry_local_order_id);
    return detail::StrategyOrderPositionLogFields{
        .position_id = group == nullptr ? 0 : group->group_id,
        .position_event = reduce_only ? "kExitSubmit" : "kEntrySubmit",
        .position_direction =
            PositionDirectionForOrderGroup(group, action, side, reduce_only),
        .order_role = detail::StrategyOrderRoleText(action, reduce_only),
        .entry_local_order_id = entry_local_order_id,
    };
  }

  [[nodiscard]] static detail::StrategyOrderPositionLogFields
  BuildFinishedOrderPositionLogFields(
      const PairRuntimeState* runtime,
      const core::StrategyOrder& order) noexcept {
    const ExecutionGroup* group =
        runtime == nullptr ? nullptr
                           : runtime->execution.FindPendingOrderByLocalOrderId(
                                 order.local_order_id);
    return detail::StrategyOrderPositionLogFields{
        .position_id = group == nullptr ? 0 : group->group_id,
        .position_direction = PositionDirectionForOrderGroup(
            group, SignalAction::kNone, order.side, order.reduce_only),
        .order_role = order.reduce_only ? "exit" : "entry",
        .entry_local_order_id =
            group == nullptr ? (order.reduce_only ? 0 : order.local_order_id)
                             : group->entry_local_order_id,
    };
  }

  Config config_;
  StrategyOptions options_;
  RawMarketState raw_market_state_;
  std::vector<PairRuntimeState> pair_runtime_by_symbol_id_;
  std::vector<const PairRuntimeState*> initialized_pair_runtimes_;
  std::vector<PairRoute> routes_by_symbol_id_;
  std::vector<OrderPriceTextStorage> order_price_texts_;
  std::vector<std::uint64_t> reserved_open_risk_slot_bits_;
  MarketUpdate last_market_update_;
  SignalDecision last_signal_decision_;
  SignalTiming last_signal_timing_;
  SignalDiagnostics last_signal_diagnostics_;
  bool last_signal_diagnostics_valid_{false};
  void* triggered_signal_observer_context_{nullptr};
  TriggeredSignalObserver triggered_signal_observer_{nullptr};
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  void* market_calc_observer_context_{nullptr};
  MarketCalcObserver market_calc_observer_{nullptr};
  std::uint64_t market_calc_row_index_{0};
#endif
  RecoveryState recovery_state_{RecoveryState::kNormal};
  std::uint64_t next_execution_parent_id_{1};
  bool stop_requested_{false};
  static constexpr double kPriceEpsilon = 1e-12;
  static constexpr double kQuantityEpsilon = 1e-12;
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
