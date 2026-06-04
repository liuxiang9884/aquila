#ifndef AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
#define AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_

#include <algorithm>
#include <array>
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
#include "strategy/lead_lag/execution_state.h"
#include "strategy/lead_lag/raw_market_state.h"
#include "strategy/lead_lag/recorders.h"
#include "strategy/lead_lag/signal.h"
#include "strategy/lead_lag/threshold.h"

namespace aquila::strategy::leadlag {

enum class PositionAccountingMode : std::uint8_t {
  kExternalOrders,
  kSyntheticSignals,
};

struct StrategyOptions {
  PositionAccountingMode position_accounting{
      PositionAccountingMode::kExternalOrders};
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
  std::int64_t lag_exchange_ns{0};
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

#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
struct StrategySignalTriggeredLogRecordForTest {
  Exchange trigger_exchange{Exchange::kGate};
  std::int32_t trigger_symbol_id{0};
  std::int64_t trigger_exchange_ns{0};
  std::int64_t trigger_local_ns{0};
  std::int64_t on_book_ticker_entry_ns{0};
  std::int64_t signal_decision_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::string_view symbol;
  std::int32_t symbol_id{0};
  PairRole role{PairRole::kNone};
  SignalAction action{SignalAction::kNone};
  OrderSide side{OrderSide::kBuy};
  bool reduce_only{false};
  std::uint64_t position_id{0};
  double raw_price{0.0};
};

struct StrategyOrderIntentLogRecordForTest {
  std::int64_t trigger_exchange_ns{0};
  std::int64_t trigger_local_ns{0};
  std::int64_t on_book_ticker_entry_ns{0};
  std::int64_t signal_decision_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::string_view symbol;
  std::int32_t symbol_id{0};
  SignalAction action{SignalAction::kNone};
  OrderSide side{OrderSide::kBuy};
  bool reduce_only{false};
  std::uint64_t position_id{0};
  double quantity{0.0};
  double raw_price{0.0};
  double order_price{0.0};
  double price{0.0};
  double target_open_notional{0.0};
  double estimated_notional{0.0};
  std::size_t active_groups{0};
};

struct StrategyOrderSubmittedLogRecordForTest {
  std::uint64_t local_order_id{0};
  Exchange trigger_exchange{Exchange::kGate};
  std::int32_t trigger_symbol_id{0};
  std::int64_t trigger_exchange_ns{0};
  std::int64_t trigger_local_ns{0};
  std::int64_t on_book_ticker_entry_ns{0};
  std::int64_t signal_decision_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::string_view symbol;
  std::int32_t symbol_id{0};
  PairRole signal_role{PairRole::kNone};
  std::string_view order_role;
  SignalAction action{SignalAction::kNone};
  OrderSide side{OrderSide::kBuy};
  bool reduce_only{false};
  std::uint64_t position_id{0};
  std::string_view position_event;
  PositionDirection position_direction{PositionDirection::kNone};
  std::uint64_t entry_local_order_id{0};
  double quantity{0.0};
  std::string_view quantity_text;
  double raw_price{0.0};
  double order_price{0.0};
  std::string_view price_text;
  std::uint32_t slippage_ticks{0};
  double price_tick{0.0};
  double target_open_notional{0.0};
  double estimated_notional{0.0};
  std::size_t active_groups{0};
  core::OrderPlaceStatus place_status{core::OrderPlaceStatus::kInvalidOrder};
};

struct StrategyOrderFinishedLogRecordForTest {
  std::uint64_t local_order_id{0};
  std::uint64_t position_id{0};
  PositionDirection position_direction{PositionDirection::kNone};
  std::string_view order_role;
  std::uint64_t entry_local_order_id{0};
  std::int64_t order_finished_local_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lag_exchange_ns{0};
};

struct StrategyOrderResponseLogRecordForTest {
  core::OrderResponseKind kind{core::OrderResponseKind::kAck};
  std::uint64_t local_order_id{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lag_exchange_ns{0};
};

struct StrategyOrderFeedbackLogRecordForTest {
  OrderFeedbackKind kind{OrderFeedbackKind::kAccepted};
  std::uint64_t local_order_id{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lag_exchange_ns{0};
};

using StrategySignalTriggeredLogObserverForTest =
    void (*)(const StrategySignalTriggeredLogRecordForTest& record) noexcept;

using StrategyOrderIntentLogObserverForTest =
    void (*)(const StrategyOrderIntentLogRecordForTest& record) noexcept;

using StrategyOrderSubmittedLogObserverForTest =
    void (*)(const StrategyOrderSubmittedLogRecordForTest& record) noexcept;

using StrategyOrderFinishedLogObserverForTest =
    void (*)(const StrategyOrderFinishedLogRecordForTest& record) noexcept;

using StrategyOrderResponseLogObserverForTest =
    void (*)(const StrategyOrderResponseLogRecordForTest& record) noexcept;

using StrategyOrderFeedbackLogObserverForTest =
    void (*)(const StrategyOrderFeedbackLogRecordForTest& record) noexcept;

[[nodiscard]] inline StrategySignalTriggeredLogObserverForTest&
StrategySignalTriggeredLogObserverSlotForTest() noexcept {
  static StrategySignalTriggeredLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline StrategyOrderIntentLogObserverForTest&
StrategyOrderIntentLogObserverSlotForTest() noexcept {
  static StrategyOrderIntentLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline StrategyOrderSubmittedLogObserverForTest&
StrategyOrderSubmittedLogObserverSlotForTest() noexcept {
  static StrategyOrderSubmittedLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline StrategyOrderFinishedLogObserverForTest&
StrategyOrderFinishedLogObserverSlotForTest() noexcept {
  static StrategyOrderFinishedLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline StrategyOrderResponseLogObserverForTest&
StrategyOrderResponseLogObserverSlotForTest() noexcept {
  static StrategyOrderResponseLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline StrategyOrderFeedbackLogObserverForTest&
StrategyOrderFeedbackLogObserverSlotForTest() noexcept {
  static StrategyOrderFeedbackLogObserverForTest observer = nullptr;
  return observer;
}

inline void SetStrategySignalTriggeredLogObserverForTest(
    StrategySignalTriggeredLogObserverForTest observer) noexcept {
  StrategySignalTriggeredLogObserverSlotForTest() = observer;
}

inline void SetStrategyOrderIntentLogObserverForTest(
    StrategyOrderIntentLogObserverForTest observer) noexcept {
  StrategyOrderIntentLogObserverSlotForTest() = observer;
}

inline void SetStrategyOrderSubmittedLogObserverForTest(
    StrategyOrderSubmittedLogObserverForTest observer) noexcept {
  StrategyOrderSubmittedLogObserverSlotForTest() = observer;
}

inline void SetStrategyOrderFinishedLogObserverForTest(
    StrategyOrderFinishedLogObserverForTest observer) noexcept {
  StrategyOrderFinishedLogObserverSlotForTest() = observer;
}

inline void SetStrategyOrderResponseLogObserverForTest(
    StrategyOrderResponseLogObserverForTest observer) noexcept {
  StrategyOrderResponseLogObserverSlotForTest() = observer;
}

inline void SetStrategyOrderFeedbackLogObserverForTest(
    StrategyOrderFeedbackLogObserverForTest observer) noexcept {
  StrategyOrderFeedbackLogObserverSlotForTest() = observer;
}

inline void NotifyStrategySignalTriggeredLogObserverForTest(
    const StrategySignalTriggeredLogRecordForTest& record) noexcept {
  StrategySignalTriggeredLogObserverForTest observer =
      StrategySignalTriggeredLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(record);
}

inline void NotifyStrategyOrderIntentLogObserverForTest(
    const StrategyOrderIntentLogRecordForTest& record) noexcept {
  StrategyOrderIntentLogObserverForTest observer =
      StrategyOrderIntentLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(record);
}

inline void NotifyStrategyOrderSubmittedLogObserverForTest(
    const StrategyOrderSubmittedLogRecordForTest& record) noexcept {
  StrategyOrderSubmittedLogObserverForTest observer =
      StrategyOrderSubmittedLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(record);
}

inline void NotifyStrategyOrderFinishedLogObserverForTest(
    const StrategyOrderFinishedLogRecordForTest& record) noexcept {
  StrategyOrderFinishedLogObserverForTest observer =
      StrategyOrderFinishedLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(record);
}

inline void NotifyStrategyOrderResponseLogObserverForTest(
    const StrategyOrderResponseLogRecordForTest& record) noexcept {
  StrategyOrderResponseLogObserverForTest observer =
      StrategyOrderResponseLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(record);
}

inline void NotifyStrategyOrderFeedbackLogObserverForTest(
    const StrategyOrderFeedbackLogRecordForTest& record) noexcept {
  StrategyOrderFeedbackLogObserverForTest observer =
      StrategyOrderFeedbackLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(record);
}
#endif

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

[[nodiscard]] inline std::int64_t StrategyLogRealtimeNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline void LogStrategySignalTriggered(
    Exchange trigger_exchange, std::int32_t trigger_symbol_id,
    const SignalTiming& timing,
    std::string_view symbol, std::int32_t symbol_id, PairRole role,
    SignalAction action, OrderSide side, bool reduce_only,
    std::uint64_t position_id, double raw_price) noexcept {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_INFO(
        "lead_lag_signal_triggered trigger_exchange={} trigger_symbol_id={} "
        "trigger_exchange_ns={} trigger_local_ns={} "
        "on_book_ticker_entry_ns={} signal_decision_ns={} "
        "lead_exchange_ns={} lag_exchange_ns={} symbol={} symbol_id={} "
        "role={} action={} side={} reduce_only={} position_id={} "
        "raw_price={:.12g}",
        magic_enum::enum_name(trigger_exchange),
        trigger_symbol_id, timing.trigger_exchange_ns, timing.trigger_local_ns,
        timing.on_book_ticker_entry_ns, timing.signal_decision_ns,
        timing.lead_exchange_ns, timing.lag_exchange_ns, symbol, symbol_id,
        magic_enum::enum_name(role), magic_enum::enum_name(action),
        magic_enum::enum_name(side), reduce_only ? "true" : "false",
        position_id, raw_price);
  }
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
          .lag_exchange_ns = timing.lag_exchange_ns,
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

inline void LogStrategyOrderIntent(
    const SignalTiming& timing, std::string_view symbol,
    std::int32_t symbol_id, SignalAction action,
    OrderSide side, bool reduce_only, std::uint64_t position_id,
    double quantity, double raw_price, double order_price,
    std::uint32_t slippage_ticks, double price_tick,
    double target_open_notional, double estimated_notional,
    std::size_t active_groups) noexcept {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_INFO(
        "lead_lag_order_intent trigger_exchange_ns={} "
        "trigger_local_ns={} on_book_ticker_entry_ns={} "
        "signal_decision_ns={} lead_exchange_ns={} lag_exchange_ns={} "
        "symbol={} symbol_id={} action={} side={} reduce_only={} "
        "position_id={} quantity={:.12g} "
        "price={:.12g} raw_price={:.12g} order_price={:.12g} "
        "slippage_ticks={} price_tick={:.12g} target_open_notional={:.12g} "
        "estimated_notional={:.12g} active_groups={}",
        timing.trigger_exchange_ns, timing.trigger_local_ns,
        timing.on_book_ticker_entry_ns, timing.signal_decision_ns,
        timing.lead_exchange_ns, timing.lag_exchange_ns, symbol, symbol_id,
        magic_enum::enum_name(action), magic_enum::enum_name(side),
        reduce_only ? "true" : "false", position_id, quantity, order_price,
        raw_price, order_price, slippage_ticks, price_tick,
        target_open_notional, estimated_notional, active_groups);
  }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderIntentLogObserverForTest(
      StrategyOrderIntentLogRecordForTest{
          .trigger_exchange_ns = timing.trigger_exchange_ns,
          .trigger_local_ns = timing.trigger_local_ns,
          .on_book_ticker_entry_ns = timing.on_book_ticker_entry_ns,
          .signal_decision_ns = timing.signal_decision_ns,
          .lead_exchange_ns = timing.lead_exchange_ns,
          .lag_exchange_ns = timing.lag_exchange_ns,
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
    std::uint64_t local_order_id, Exchange trigger_exchange,
    std::int32_t trigger_symbol_id,
    const SignalTiming& timing, std::string_view symbol, std::int32_t symbol_id,
    PairRole signal_role, std::string_view order_role, SignalAction action,
    OrderSide side, bool reduce_only,
    const StrategyOrderPositionLogFields& position, double quantity,
    std::string_view quantity_text, double raw_price, double order_price,
    std::string_view price_text, std::uint32_t slippage_ticks,
    double price_tick, double target_open_notional, double estimated_notional,
    std::size_t active_groups, core::OrderPlaceStatus place_status) noexcept {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_INFO(
        "lead_lag_order_submitted local_order_id={} trigger_exchange={} "
        "trigger_symbol_id={} trigger_exchange_ns={} "
        "trigger_local_ns={} on_book_ticker_entry_ns={} "
        "signal_decision_ns={} lead_exchange_ns={} lag_exchange_ns={} "
        "symbol={} symbol_id={} signal_role={} order_role={} action={} "
        "side={} reduce_only={} "
        "position_id={} position_event={} position_direction={} "
        "entry_local_order_id={} quantity={:.12g} quantity_text={} "
        "raw_price={:.12g} order_price={:.12g} price_text={} "
        "slippage_ticks={} price_tick={:.12g} target_open_notional={:.12g} "
        "estimated_notional={:.12g} active_groups={} place_status={}",
        local_order_id, magic_enum::enum_name(trigger_exchange),
        trigger_symbol_id,
        timing.trigger_exchange_ns, timing.trigger_local_ns,
        timing.on_book_ticker_entry_ns, timing.signal_decision_ns,
        timing.lead_exchange_ns, timing.lag_exchange_ns, symbol, symbol_id,
        magic_enum::enum_name(signal_role), order_role,
        magic_enum::enum_name(action), magic_enum::enum_name(side),
        reduce_only ? "true" : "false", position.position_id,
        position.position_event,
        magic_enum::enum_name(position.position_direction),
        position.entry_local_order_id, quantity, quantity_text, raw_price,
        order_price, price_text, slippage_ticks, price_tick,
        target_open_notional, estimated_notional, active_groups,
        magic_enum::enum_name(place_status));
  }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderSubmittedLogObserverForTest(
      StrategyOrderSubmittedLogRecordForTest{
          .local_order_id = local_order_id,
          .trigger_exchange = trigger_exchange,
          .trigger_symbol_id = trigger_symbol_id,
          .trigger_exchange_ns = timing.trigger_exchange_ns,
          .trigger_local_ns = timing.trigger_local_ns,
          .on_book_ticker_entry_ns = timing.on_book_ticker_entry_ns,
          .signal_decision_ns = timing.signal_decision_ns,
          .lead_exchange_ns = timing.lead_exchange_ns,
          .lag_exchange_ns = timing.lag_exchange_ns,
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
    std::string_view reason, const SignalTiming& timing, std::string_view symbol,
    std::int32_t symbol_id, SignalAction action, OrderSide side,
    bool reduce_only, std::uint64_t position_id, double quantity,
    double raw_price, double order_price,
    std::uint32_t slippage_ticks, double price_tick,
    double target_open_notional, double estimated_notional,
    double gross_before = 0.0, double gross_after = 0.0,
    double max_gross_notional = 0.0, std::uint64_t local_order_id = 0,
    std::string_view place_status = "-") noexcept {
  if (::nova::kLogManager.logger() == nullptr) {
    return;
  }
  NOVA_WARNING(
      "lead_lag_order_intent_rejected reason={} trigger_exchange_ns={} "
      "trigger_local_ns={} on_book_ticker_entry_ns={} signal_decision_ns={} "
      "lead_exchange_ns={} lag_exchange_ns={} symbol={} symbol_id={} "
      "action={} side={} reduce_only={} position_id={} quantity={:.12g} "
      "price={:.12g} raw_price={:.12g} "
      "order_price={:.12g} slippage_ticks={} price_tick={:.12g} "
      "target_open_notional={:.12g} estimated_notional={:.12g} "
      "gross_before={:.12g} gross_after={:.12g} max_gross_notional={:.12g} "
      "local_order_id={} place_status={}",
      reason, timing.trigger_exchange_ns, timing.trigger_local_ns,
      timing.on_book_ticker_entry_ns, timing.signal_decision_ns,
      timing.lead_exchange_ns, timing.lag_exchange_ns, symbol, symbol_id,
      magic_enum::enum_name(action), magic_enum::enum_name(side),
      reduce_only ? "true" : "false", position_id, quantity, order_price,
      raw_price, order_price, slippage_ticks, price_tick, target_open_notional,
      estimated_notional, gross_before, gross_after, max_gross_notional,
      local_order_id, place_status);
}

inline void LogStrategyOrderResponse(
    const core::OrderResponseEvent& event, const core::StrategyOrder* order,
    const SignalTiming& market_timing) noexcept {
  const core::StrategyOrderTimingSnapshot timing =
      order == nullptr ? core::StrategyOrderTimingSnapshot{}
                       : core::MakeStrategyOrderTimingSnapshot(*order);
  const std::int64_t exchange_to_local_ns =
      core::LatencyDeltaNs(event.local_receive_ns, event.exchange_ns);
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_INFO(
        "lead_lag_order_response kind={} local_order_id={} "
        "exchange_order_id={} local_receive_ns={} exchange_ns={} "
        "exchange_to_local_ns={} ack_rtt_ns={} response_rtt_ns={} "
        "lead_exchange_ns={} lag_exchange_ns={}",
        magic_enum::enum_name(event.kind), event.local_order_id,
        event.exchange_order_id, event.local_receive_ns, event.exchange_ns,
        exchange_to_local_ns, timing.ack_rtt_ns, timing.response_rtt_ns,
        market_timing.lead_exchange_ns, market_timing.lag_exchange_ns);
  }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderResponseLogObserverForTest(
      StrategyOrderResponseLogRecordForTest{
          .kind = event.kind,
          .local_order_id = event.local_order_id,
          .lead_exchange_ns = market_timing.lead_exchange_ns,
          .lag_exchange_ns = market_timing.lag_exchange_ns,
      });
#endif
}

inline void LogStrategyOrderFeedback(
    const OrderFeedbackEvent& event,
    const SignalTiming& market_timing) noexcept {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_INFO(
        "lead_lag_order_feedback kind={} local_order_id={} "
        "exchange_order_id={} "
        "cumulative_filled_quantity={:.12g} left_quantity={:.12g} "
        "cancelled_quantity={:.12g} fill_price={:.12g} role={} "
        "finish_reason={} reject_reason={} exchange_update_ns={} "
        "local_receive_ns={} lead_exchange_ns={} lag_exchange_ns={}",
        magic_enum::enum_name(event.kind), event.local_order_id,
        event.exchange_order_id, event.cumulative_filled_quantity,
        event.left_quantity, event.cancelled_quantity, event.fill_price,
        magic_enum::enum_name(event.role),
        magic_enum::enum_name(event.finish_reason),
        magic_enum::enum_name(event.reject_reason), event.exchange_update_ns,
        event.local_receive_ns, market_timing.lead_exchange_ns,
        market_timing.lag_exchange_ns);
  }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderFeedbackLogObserverForTest(
      StrategyOrderFeedbackLogRecordForTest{
          .kind = event.kind,
          .local_order_id = event.local_order_id,
          .lead_exchange_ns = market_timing.lead_exchange_ns,
          .lag_exchange_ns = market_timing.lag_exchange_ns,
      });
#endif
}

inline void LogStrategyFeedbackContinuityLost(
    const OrderFeedbackEvent& event) noexcept {
  if (::nova::kLogManager.logger() == nullptr) {
    return;
  }
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
  if (::nova::kLogManager.logger() == nullptr) {
    return;
  }
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
  if (::nova::kLogManager.logger() == nullptr) {
    return;
  }
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
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_INFO(
        "lead_lag_order_finished local_order_id={} symbol_id={} symbol={} "
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
        order.local_order_id, order.symbol_id, order.symbol,
        magic_enum::enum_name(order.status),
        order.reduce_only ? "true" : "false", position.position_id,
        magic_enum::enum_name(position.position_direction), position.order_role,
        position.entry_local_order_id, position.order_finished_local_ns,
        order.quantity, order.cumulative_filled_quantity,
        order.AverageFillPrice(), order.last_fill_price,
        order.exchange_order_id, active_groups, timing.request_send_local_ns,
        timing.ack_local_receive_ns, timing.response_local_receive_ns,
        timing.ack_exchange_ns, timing.response_exchange_ns,
        timing.accepted_exchange_ns, timing.finish_exchange_ns,
        timing.ack_rtt_ns, timing.response_rtt_ns,
        timing.ack_exchange_to_local_ns, timing.response_exchange_to_local_ns,
        timing.exchange_lifecycle_ns, market_timing.lead_exchange_ns,
        market_timing.lag_exchange_ns);
  }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderFinishedLogObserverForTest(
      StrategyOrderFinishedLogRecordForTest{
          .local_order_id = order.local_order_id,
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
  explicit Strategy(Config config, StrategyOptions options = {})
      : config_(std::move(config)), options_(options) {
    raw_market_state_.Reset(config_);
    InitPairRuntimeStates();
  }

  Strategy(const Strategy&) = delete;
  Strategy& operator=(const Strategy&) = delete;
  Strategy(Strategy&&) noexcept = default;
  Strategy& operator=(Strategy&&) noexcept = default;

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
      return;
    }

    const std::int64_t now_ns = BookTickerEventTimeNs(ticker);
    if (last_market_update_.both_sides_valid) {
      runtime->alignment.OnPairedRawBbo(now_ns, market->lead.latest_quote,
                                        market->lag.latest_quote);
    }

    const AlignmentPhase previous_phase = runtime->alignment.phase();
    const AlignmentPhase phase = runtime->alignment.UpdatePhase(
        now_ns, market->lead.has_quote, market->lag.has_quote);
    if (phase != AlignmentPhase::kActive) {
      return;
    }

    if (previous_phase != AlignmentPhase::kActive) {
      const ActiveSeed seed = market->SelectActiveSeed(
          last_market_update_.role, last_market_update_.price_changed);
      const ActiveTransition transition = runtime->alignment.EnterActive(
          now_ns, seed, last_market_update_.role);
      if (!transition.valid) {
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
    detail::LogStrategyOrderFeedback(event, market_timing);
    ApplyFinishedOrder(event.local_order_id, context, market_timing);
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

    if (recovery_state_ != RecoveryState::kReconciling ||
        !AllInitializedRuntimesReconciling()) {
      MarkNeedsReconcile();
      for (PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
        if (runtime.initialized) {
          runtime.execution.MarkNeedsReconcile();
        }
      }
      return false;
    }

    bool all_recovered = true;
    for (PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
      if (runtime.initialized) {
        all_recovered =
            runtime.execution.ApplyRecoveryResult(result) && all_recovered;
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
    AlignmentState alignment;
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
        .lag_exchange_ns = route->market->lag.latest_quote.exchange_ns,
    };
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
      price_text_slot_count += pair.execute.parallel;
      PairRuntimeState& runtime =
          pair_runtime_by_symbol_id_[static_cast<std::size_t>(pair.symbol_id)];
      runtime.initialized = true;
      runtime.pair = pair;
      runtime.order_decimal = *order_decimal;
      runtime.alignment.Init(AlignmentConfig{
          .drift_period_ns = pair.trigger.drift_period_ns,
          .stats_window_ns = pair.bbo_record.stats_window_ns,
          .drift_warmup_ns = pair.trigger.drift_warmup_ns,
          .drift_min_samples = pair.trigger.drift_min_samples,
          .initial_capacity = pair.capacity.spread_window_capacity,
      });
      runtime.recorder.Init(pair);
      runtime.threshold.Init(pair);
      runtime.execution.Init(pair.execute.parallel);
      route->runtime = &runtime;
    }
    order_price_texts_.resize(price_text_slot_count);
  }

  [[nodiscard]] static bool RuntimeConfigReady(
      const PairConfig& pair) noexcept {
    return pair.symbol_id >= 0 && pair.trigger.drift_period_ns > 0 &&
           pair.bbo_record.window_ns > 0 &&
           pair.bbo_record.stats_window_ns > 0 &&
           pair.trigger.quantile.up_max > pair.trigger.quantile.up_min &&
           pair.trigger.quantile.down_max > pair.trigger.quantile.down_min;
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

  void MarkNeedsReconcile() noexcept {
    if (recovery_state_ != RecoveryState::kManualIntervention) {
      recovery_state_ = RecoveryState::kDegradedNeedsReconcile;
    }
  }

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

    last_signal_decision_ =
        SignalEngine::OnLeadTick(runtime->pair, runtime->execution,
                                 SignalMarket{
                                     .lead = drifted_lead,
                                     .lag = market.lag.latest_quote,
                                     .recorder = recorder,
                                 },
                                 threshold, alignment);
    if (last_signal_decision_.triggered) {
      last_signal_timing_ = SignalTiming{
          .trigger_exchange_ns = trigger_ticker.exchange_ns,
          .trigger_local_ns = trigger_ticker.local_ns,
          .on_book_ticker_entry_ns = on_book_ticker_entry_ns,
          .signal_decision_ns = detail::StrategyLogRealtimeNowNs(),
          .lead_exchange_ns = market.lead.latest_quote.exchange_ns,
          .lag_exchange_ns = market.lag.latest_quote.exchange_ns};
      last_signal_diagnostics_ = BuildSignalDiagnostics(
          *runtime, market, drifted_lead, recorder, alignment, threshold);
      last_signal_diagnostics_valid_ = true;
      detail::LogStrategySignalTriggered(
          trigger_ticker.exchange, trigger_ticker.symbol_id, last_signal_timing_,
          runtime->pair.symbol, runtime->pair.symbol_id, PairRole::kLead,
          last_signal_decision_.action,
          last_signal_decision_.intent.side,
          last_signal_decision_.intent.reduce_only,
          last_signal_decision_.group_id, last_signal_decision_.intent.price);
    }
    if (SyntheticPositionAccounting()) {
      ApplySyntheticSignal(runtime, last_signal_decision_);
    } else {
      SubmitExternalSignal(runtime, trigger_ticker, PairRole::kLead, context);
    }
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

    last_signal_decision_ =
        SignalEngine::OnLagTick(runtime->pair, runtime->execution,
                                SignalMarket{
                                    .lead = runtime->drifted_lead,
                                    .lag = market.lag.latest_quote,
                                    .recorder = recorder,
                                },
                                threshold);
    if (last_signal_decision_.triggered) {
      const AlignmentSnapshot alignment = runtime->alignment.Snapshot();
      last_signal_timing_ = SignalTiming{
          .trigger_exchange_ns = trigger_ticker.exchange_ns,
          .trigger_local_ns = trigger_ticker.local_ns,
          .on_book_ticker_entry_ns = on_book_ticker_entry_ns,
          .signal_decision_ns = detail::StrategyLogRealtimeNowNs(),
          .lead_exchange_ns = market.lead.latest_quote.exchange_ns,
          .lag_exchange_ns = market.lag.latest_quote.exchange_ns};
      last_signal_diagnostics_ =
          BuildSignalDiagnostics(*runtime, market, runtime->drifted_lead,
                                 recorder, alignment, threshold);
      last_signal_diagnostics_valid_ = true;
      detail::LogStrategySignalTriggered(
          trigger_ticker.exchange, trigger_ticker.symbol_id, last_signal_timing_,
          runtime->pair.symbol, runtime->pair.symbol_id, PairRole::kLag,
          last_signal_decision_.action,
          last_signal_decision_.intent.side,
          last_signal_decision_.intent.reduce_only,
          last_signal_decision_.group_id, last_signal_decision_.intent.price);
    }
    if (SyntheticPositionAccounting()) {
      ApplySyntheticSignal(runtime, last_signal_decision_);
    } else {
      SubmitExternalSignal(runtime, trigger_ticker, PairRole::kLag, context);
    }
  }

  [[nodiscard]] bool SyntheticPositionAccounting() const noexcept {
    return options_.position_accounting ==
           PositionAccountingMode::kSyntheticSignals;
  }

  static void ApplySyntheticSignal(PairRuntimeState* runtime,
                                   const SignalDecision& decision) noexcept {
    if (!decision.triggered) {
      return;
    }
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

  template <typename ContextT>
  void SubmitExternalSignal(PairRuntimeState* runtime,
                            const BookTicker& trigger_ticker,
                            PairRole signal_role, ContextT& context) noexcept {
    if (runtime == nullptr || !last_signal_decision_.triggered) {
      return;
    }

    const InstrumentMetadata& instrument = runtime->pair.lag_instrument;
    const double raw_order_price = last_signal_decision_.intent.price;
    const std::uint32_t slippage_ticks = SlippageTicksForAction(
        runtime->pair.execute, last_signal_decision_.action);
    const double rounded_order_price = SlippedRoundedOrderPrice(
        raw_order_price, instrument, last_signal_decision_.intent.side,
        slippage_ticks);
    const std::string_view symbol =
        instrument.exchange_symbol.empty()
            ? std::string_view(runtime->pair.symbol)
            : std::string_view(instrument.exchange_symbol);
    if (rounded_order_price <= 0.0) {
      detail::LogStrategyOrderIntentRejected(
          "invalid_price", last_signal_timing_, symbol,
          runtime->pair.symbol_id, last_signal_decision_.action,
          last_signal_decision_.intent.side,
          last_signal_decision_.intent.reduce_only,
          last_signal_decision_.group_id, 0, raw_order_price,
          rounded_order_price, slippage_ticks, instrument.price_tick,
          runtime->pair.execute.open_notional, 0.0);
      return;
    }

    std::int64_t price_units = 0;
    if (!DecimalUnitsFromValue(rounded_order_price,
                               instrument.price_decimal_places, &price_units)) {
      detail::LogStrategyOrderIntentRejected(
          "order_text_slot_full", last_signal_timing_, symbol,
          runtime->pair.symbol_id, last_signal_decision_.action,
          last_signal_decision_.intent.side,
          last_signal_decision_.intent.reduce_only,
          last_signal_decision_.group_id, 0, raw_order_price,
          rounded_order_price, slippage_ticks, instrument.price_tick,
          runtime->pair.execute.open_notional, 0.0);
      return;
    }
    const double order_price =
        DecimalUnitsToValue(price_units, instrument.price_decimal_places);

    double quantity = 0.0;
    std::int64_t quantity_units = 0;
    ExecutionGroup* close_group = nullptr;
    if (last_signal_decision_.intent.reduce_only) {
      close_group =
          runtime->execution.FindGroupById(last_signal_decision_.group_id);
      if (close_group == nullptr) {
        return;
      }
      quantity = AbsolutePositionQuantity(*close_group);
      if (!DecimalUnitsFromValue(quantity, instrument.quantity_decimal_places,
                                 &quantity_units)) {
        quantity = 0.0;
      } else {
        quantity = DecimalUnitsToValue(quantity_units,
                                       instrument.quantity_decimal_places);
      }
    } else {
      const OpenOrderQuantity open_quantity =
          CalculateOpenOrderQuantity(*runtime, price_units);
      quantity = open_quantity.quantity;
      quantity_units = open_quantity.quantity_units;
    }
    if (quantity <= kQuantityEpsilon) {
      detail::LogStrategyOrderIntentRejected(
          "zero_quantity", last_signal_timing_, symbol,
          runtime->pair.symbol_id, last_signal_decision_.action,
          last_signal_decision_.intent.side,
          last_signal_decision_.intent.reduce_only,
          last_signal_decision_.group_id, quantity, raw_order_price,
          order_price, slippage_ticks, instrument.price_tick,
          runtime->pair.execute.open_notional, 0.0);
      return;
    }

    const double order_notional =
        OrderNotional(quantity, order_price, instrument);
    if (!last_signal_decision_.intent.reduce_only &&
        !GlobalRiskAllowsOpen(quantity, order_notional)) {
      const GlobalRiskTotals totals = CurrentGlobalRiskTotals();
      detail::LogStrategyOrderIntentRejected(
          "risk_limit", last_signal_timing_, symbol, runtime->pair.symbol_id,
          last_signal_decision_.action, last_signal_decision_.intent.side,
          last_signal_decision_.intent.reduce_only,
          last_signal_decision_.group_id, quantity, raw_order_price,
          order_price, slippage_ticks, instrument.price_tick,
          runtime->pair.execute.open_notional, order_notional,
          totals.gross_notional, totals.gross_notional + order_notional,
          config_.risk.max_gross_notional);
      RejectSignal(SignalRejectReason::kRiskLimit);
      return;
    }

    OrderPriceTextStorage* order_text_storage =
        AcquireOrderText(price_units, instrument.price_decimal_places,
                         quantity_units, instrument.quantity_decimal_places);
    if (order_text_storage == nullptr) {
      detail::LogStrategyOrderIntentRejected(
          "order_text_slot_full", last_signal_timing_, symbol,
          runtime->pair.symbol_id, last_signal_decision_.action,
          last_signal_decision_.intent.side,
          last_signal_decision_.intent.reduce_only,
          last_signal_decision_.group_id, quantity, raw_order_price,
          order_price, slippage_ticks, instrument.price_tick,
          runtime->pair.execute.open_notional, order_notional);
      return;
    }
    const std::string_view quantity_text = order_text_storage->quantity_view();
    const std::string_view price_text = order_text_storage->price_view();

    detail::LogStrategyOrderIntent(
        last_signal_timing_, symbol, runtime->pair.symbol_id,
        last_signal_decision_.action, last_signal_decision_.intent.side,
        last_signal_decision_.intent.reduce_only,
        last_signal_decision_.group_id, quantity, raw_order_price, order_price,
        slippage_ticks, instrument.price_tick,
        runtime->pair.execute.open_notional, order_notional,
        runtime->execution.active_group_count());

    const core::OrderPlaceResult placed =
        context.PlaceOrder(core::OrderCreateRequest{
            .exchange = last_signal_decision_.intent.exchange,
            .symbol_id = last_signal_decision_.intent.symbol_id,
            .symbol = symbol,
            .side = last_signal_decision_.intent.side,
            .order_type = OrderType::kLimit,
            .time_in_force = TimeInForce::kImmediateOrCancel,
            .quantity = quantity,
            .quantity_text = quantity_text,
            .price_text = price_text,
            .reduce_only = last_signal_decision_.intent.reduce_only,
        });
    if (placed.local_order_id == 0) {
      detail::LogStrategyOrderIntentRejected(
          "place_local_rejected", last_signal_timing_, symbol,
          runtime->pair.symbol_id, last_signal_decision_.action,
          last_signal_decision_.intent.side,
          last_signal_decision_.intent.reduce_only,
          last_signal_decision_.group_id, quantity, raw_order_price,
          order_price, slippage_ticks, instrument.price_tick,
          runtime->pair.execute.open_notional, order_notional, 0.0, 0.0, 0.0,
          placed.local_order_id, magic_enum::enum_name(placed.status));
      ReleaseOrderPriceText(order_text_storage);
      return;
    }
    order_text_storage->local_order_id = placed.local_order_id;

    if (placed.status == core::OrderPlaceStatus::kOk) {
      const bool tracked =
          OnExternalOrderAccepted(runtime, close_group, placed.local_order_id);
      if (tracked) {
        const ExecutionGroup* submitted_group =
            runtime->execution.FindPendingOrderByLocalOrderId(
                placed.local_order_id);
        const detail::StrategyOrderPositionLogFields position_log =
            BuildSubmittedOrderPositionLogFields(
                submitted_group, last_signal_decision_.action,
                last_signal_decision_.intent.side,
                last_signal_decision_.intent.reduce_only,
                placed.local_order_id);
        detail::LogStrategyOrderSubmitted(
            placed.local_order_id, trigger_ticker.exchange,
            trigger_ticker.symbol_id, last_signal_timing_, symbol,
            runtime->pair.symbol_id, signal_role,
            detail::StrategyOrderRoleText(
                last_signal_decision_.action,
                last_signal_decision_.intent.reduce_only),
            last_signal_decision_.action, last_signal_decision_.intent.side,
            last_signal_decision_.intent.reduce_only, position_log, quantity,
            quantity_text, raw_order_price, order_price, price_text,
            slippage_ticks, instrument.price_tick,
            runtime->pair.execute.open_notional, order_notional,
            runtime->execution.active_group_count(), placed.status);
      }
      if (tracked && !last_signal_decision_.intent.reduce_only) {
        ReserveOpenRisk(order_text_storage, quantity, order_notional);
      }
      return;
    }

    detail::LogStrategyOrderIntentRejected(
        "place_local_rejected", last_signal_timing_, symbol,
        runtime->pair.symbol_id, last_signal_decision_.action,
        last_signal_decision_.intent.side,
        last_signal_decision_.intent.reduce_only,
        last_signal_decision_.group_id, quantity, raw_order_price, order_price,
        slippage_ticks, instrument.price_tick,
        runtime->pair.execute.open_notional, order_notional, 0.0, 0.0, 0.0,
        placed.local_order_id, magic_enum::enum_name(placed.status));
    RollbackRejectedSubmit(runtime, close_group, placed.local_order_id,
                           context);
  }

  [[nodiscard]] bool OnExternalOrderAccepted(
      PairRuntimeState* runtime, ExecutionGroup* close_group,
      std::uint64_t local_order_id) noexcept {
    ExecutionGroup* group = close_group;
    if (last_signal_decision_.intent.reduce_only) {
      if (group == nullptr ||
          !runtime->execution.StartCloseOrder(*group, local_order_id)) {
        return false;
      }
    } else {
      group = runtime->execution.StartOpenOrder(local_order_id);
      if (group == nullptr) {
        return false;
      }
      last_signal_decision_.group_id = group->group_id;
    }
    UpdateSubmittedSignalDiagnostics(runtime, group);
    return true;
  }

  template <typename ContextT>
  void RollbackRejectedSubmit(PairRuntimeState* runtime,
                              ExecutionGroup* close_group,
                              std::uint64_t local_order_id,
                              ContextT& context) noexcept {
    if (last_signal_decision_.intent.reduce_only) {
      if (close_group != nullptr) {
        [[maybe_unused]] const bool started =
            runtime->execution.StartCloseOrder(*close_group, local_order_id);
      }
    } else {
      [[maybe_unused]] ExecutionGroup* group =
          runtime->execution.StartOpenOrder(local_order_id);
    }
    [[maybe_unused]] const ExecutionApplyResult applied =
        runtime->execution.ApplySubmitRejected(local_order_id);
    if (context.RetireFinishedOrder(local_order_id)) {
      EraseOrderPriceText(local_order_id);
    }
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
  void ApplyFinishedOrder(std::uint64_t local_order_id, ContextT& context,
                          const SignalTiming& market_timing) noexcept {
    if (local_order_id == 0) {
      return;
    }
    const core::StrategyOrder* order = context.FindOrder(local_order_id);
    if (order == nullptr || !order->is_finished) {
      return;
    }
    PairRuntimeState* runtime = MutableRuntime(order->symbol_id);
    if (runtime != nullptr) {
      const detail::StrategyOrderPositionLogFields position_log =
          BuildFinishedOrderPositionLogFields(runtime, *order);
      [[maybe_unused]] const ExecutionApplyResult applied =
          runtime->execution.ApplyTerminalOrder(*order,
                                                runtime->pair.lag_instrument);
      detail::LogStrategyOrderFinished(*order, position_log,
                                       runtime->execution.active_group_count(),
                                       market_timing);
    } else {
      detail::LogStrategyOrderFinished(
          *order, BuildFinishedOrderPositionLogFields(nullptr, *order), 0,
          market_timing);
    }
    if (context.RetireFinishedOrder(local_order_id)) {
      EraseOrderPriceText(local_order_id);
    }
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
    for (const PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
      if (!runtime.initialized) {
        continue;
      }
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
    for (const OrderPriceTextStorage& storage : order_price_texts_) {
      if (!storage.active ||
          storage.reserved_open_quantity <= kQuantityEpsilon) {
        continue;
      }
      totals.gross_notional += storage.reserved_open_notional;
      totals.holding_position += storage.reserved_open_quantity;
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
      const ExecuteConfig& execute, SignalAction action) noexcept {
    switch (action) {
      case SignalAction::kOpenLong:
      case SignalAction::kOpenShort:
        return execute.open_slippage;
      case SignalAction::kCloseLong:
      case SignalAction::kCloseShort:
      case SignalAction::kStoplossLong:
      case SignalAction::kStoplossShort:
        return execute.close_slippage;
      case SignalAction::kNone:
        return 0;
    }
    return 0;
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

  static void ReserveOpenRisk(OrderPriceTextStorage* storage, double quantity,
                              double notional) noexcept {
    if (storage == nullptr || quantity <= kQuantityEpsilon ||
        !std::isfinite(notional) || notional <= 0.0) {
      return;
    }
    storage->reserved_open_quantity = quantity;
    storage->reserved_open_notional = notional;
  }

  static void ReleaseOrderPriceText(OrderPriceTextStorage* storage) noexcept {
    if (storage == nullptr) {
      return;
    }
    storage->local_order_id = 0;
    storage->price_size = 0;
    storage->quantity_size = 0;
    storage->reserved_open_quantity = 0.0;
    storage->reserved_open_notional = 0.0;
    storage->active = false;
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
  std::vector<PairRoute> routes_by_symbol_id_;
  std::vector<OrderPriceTextStorage> order_price_texts_;
  MarketUpdate last_market_update_;
  SignalDecision last_signal_decision_;
  SignalTiming last_signal_timing_;
  SignalDiagnostics last_signal_diagnostics_;
  bool last_signal_diagnostics_valid_{false};
  RecoveryState recovery_state_{RecoveryState::kNormal};
  bool stop_requested_{false};
  static constexpr double kPriceEpsilon = 1e-12;
  static constexpr double kQuantityEpsilon = 1e-12;
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
