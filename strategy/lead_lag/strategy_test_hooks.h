#ifndef AQUILA_STRATEGY_LEAD_LAG_STRATEGY_TEST_HOOKS_H_
#define AQUILA_STRATEGY_LEAD_LAG_STRATEGY_TEST_HOOKS_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/common/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "strategy/lead_lag/signal.h"

namespace aquila::strategy::leadlag {

enum class FreshnessRejectReason : std::uint8_t;
enum class PositionDirection : std::uint8_t;

enum class StrategySubmitStageForTest : std::uint8_t {
  kPricePrepared,
  kSignalDecisionLogged,
  kFreshnessChecked,
  kQuantityPrepared,
  kRoutesRefreshed,
  kRoutesSelected,
  kRiskChecked,
  kOrderIntentLogged,
  kExecutionGroupReady,
  kBeforeAcquireText,
  kAfterAcquireText,
  kBeforePlaceOrder,
  kAfterPlaceOrder,
  kAfterSubmitResult,
  kSubmitDone,
  kCount,
};

namespace detail {

struct StrategySignalTriggeredLogRecordForTest {
  Exchange trigger_exchange{Exchange::kGate};
  std::int32_t trigger_symbol_id{0};
  std::int64_t trigger_exchange_ns{0};
  std::int64_t trigger_local_ns{0};
  std::int64_t on_book_ticker_entry_ns{0};
  std::int64_t signal_decision_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lead_local_ns{0};
  std::int64_t signal_lead_id{0};
  std::int64_t lead_freshness_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::int64_t lag_local_ns{0};
  std::int64_t signal_lag_id{0};
  std::int64_t lag_freshness_ns{0};
  std::string_view symbol;
  std::int32_t symbol_id{0};
  PairRole role{PairRole::kNone};
  SignalAction action{SignalAction::kNone};
  OrderSide side{OrderSide::kBuy};
  bool reduce_only{false};
  std::uint64_t position_id{0};
  double raw_price{0.0};
};

struct StrategySignalDecisionLogRecordForTest {
  std::int64_t trigger_exchange_ns{0};
  std::int64_t trigger_local_ns{0};
  std::int64_t on_book_ticker_entry_ns{0};
  std::int64_t signal_decision_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lead_local_ns{0};
  std::int64_t signal_lead_id{0};
  std::int64_t lead_freshness_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::int64_t lag_local_ns{0};
  std::int64_t signal_lag_id{0};
  std::int64_t lag_freshness_ns{0};
  std::string_view symbol;
  std::int32_t symbol_id{0};
  SignalAction action{SignalAction::kNone};
  OrderSide side{OrderSide::kBuy};
  bool reduce_only{false};
  std::string_view decision;
  double raw_price{0.0};
  double current_order_price{0.0};
  double reference_order_price{0.0};
  double entry_buffer_pct{0.0};
  double close_buffer_pct{0.0};
};

struct StrategyOrderIntentLogRecordForTest {
  std::int64_t trigger_exchange_ns{0};
  std::int64_t trigger_local_ns{0};
  std::int64_t on_book_ticker_entry_ns{0};
  std::int64_t signal_decision_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lead_local_ns{0};
  std::int64_t signal_lead_id{0};
  std::int64_t lead_freshness_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::int64_t lag_local_ns{0};
  std::int64_t signal_lag_id{0};
  std::int64_t lag_freshness_ns{0};
  std::uint64_t max_lead_freshness_ns{0};
  std::uint64_t max_lag_freshness_ns{0};
  bool freshness_guard_pass{true};
  FreshnessRejectReason freshness_reject_reason{};
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

struct StrategyOrderIntentRejectedLogRecordForTest {
  std::string_view reason;
  std::string_view symbol;
  std::int32_t symbol_id{0};
  SignalAction action{SignalAction::kNone};
  OrderSide side{OrderSide::kBuy};
  bool reduce_only{false};
  double quantity{0.0};
  double raw_price{0.0};
  double order_price{0.0};
  double price_tick{0.0};
  double estimated_notional{0.0};
};

struct StrategyOrderSubmittedLogRecordForTest {
  std::uint64_t local_order_id{0};
  std::uint64_t parent_id{0};
  std::uint16_t route_id{core::kAutoGatewayRoute};
  Exchange trigger_exchange{Exchange::kGate};
  std::int32_t trigger_symbol_id{0};
  std::int64_t trigger_exchange_ns{0};
  std::int64_t trigger_local_ns{0};
  std::int64_t on_book_ticker_entry_ns{0};
  std::int64_t signal_decision_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lead_local_ns{0};
  std::int64_t signal_lead_id{0};
  std::int64_t lead_freshness_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::int64_t lag_local_ns{0};
  std::int64_t signal_lag_id{0};
  std::int64_t lag_freshness_ns{0};
  std::uint64_t max_lead_freshness_ns{0};
  std::uint64_t max_lag_freshness_ns{0};
  bool freshness_guard_pass{true};
  FreshnessRejectReason freshness_reject_reason{};
  std::string_view symbol;
  std::int32_t symbol_id{0};
  PairRole signal_role{PairRole::kNone};
  std::string_view order_role;
  SignalAction action{SignalAction::kNone};
  OrderSide side{OrderSide::kBuy};
  bool reduce_only{false};
  std::uint64_t position_id{0};
  std::string_view position_event;
  PositionDirection position_direction{};
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
  std::uint64_t parent_id{0};
  std::uint16_t route_id{core::kAutoGatewayRoute};
  std::uint64_t position_id{0};
  PositionDirection position_direction{};
  std::string_view order_role;
  std::uint64_t entry_local_order_id{0};
  std::int64_t order_finished_local_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lag_exchange_ns{0};
};

struct StrategyOrderResponseLogRecordForTest {
  core::OrderResponseKind kind{core::OrderResponseKind::kAck};
  std::uint64_t local_order_id{0};
  std::uint64_t parent_id{0};
  std::uint16_t route_id{core::kAutoGatewayRoute};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::string_view book_ticker_id_prefix;
  std::int64_t lead_book_ticker_id{0};
  std::int64_t lag_book_ticker_id{0};
};

struct StrategyOrderFeedbackLogRecordForTest {
  OrderFeedbackKind kind{OrderFeedbackKind::kAccepted};
  std::uint64_t local_order_id{0};
  std::uint64_t parent_id{0};
  std::uint16_t route_id{core::kAutoGatewayRoute};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::string_view book_ticker_id_prefix;
  std::int64_t lead_book_ticker_id{0};
  std::int64_t lag_book_ticker_id{0};
};

struct StrategySubmitStageRecordForTest {
  StrategySubmitStageForTest stage{StrategySubmitStageForTest::kPricePrepared};
  std::uint64_t parent_id{0};
  std::uint64_t local_order_id{0};
  std::uint16_t route_id{core::kAutoGatewayRoute};
  std::uint32_t route_index{0};
  std::uint32_t submission_route_count{0};
};

using StrategySignalTriggeredLogObserverForTest =
    void (*)(const StrategySignalTriggeredLogRecordForTest& record) noexcept;

using StrategySignalDecisionLogObserverForTest =
    void (*)(const StrategySignalDecisionLogRecordForTest& record) noexcept;

using StrategyOrderIntentLogObserverForTest =
    void (*)(const StrategyOrderIntentLogRecordForTest& record) noexcept;

using StrategyOrderIntentRejectedLogObserverForTest = void (*)(
    const StrategyOrderIntentRejectedLogRecordForTest& record) noexcept;

using StrategyOrderSubmittedLogObserverForTest =
    void (*)(const StrategyOrderSubmittedLogRecordForTest& record) noexcept;

using StrategyOrderFinishedLogObserverForTest =
    void (*)(const StrategyOrderFinishedLogRecordForTest& record) noexcept;

using StrategyOrderResponseLogObserverForTest =
    void (*)(const StrategyOrderResponseLogRecordForTest& record) noexcept;

using StrategyOrderFeedbackLogObserverForTest =
    void (*)(const StrategyOrderFeedbackLogRecordForTest& record) noexcept;

using StrategySubmitStageObserverForTest =
    void (*)(const StrategySubmitStageRecordForTest& record) noexcept;

[[nodiscard]] inline StrategySignalTriggeredLogObserverForTest&
StrategySignalTriggeredLogObserverSlotForTest() noexcept {
  static StrategySignalTriggeredLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline StrategySignalDecisionLogObserverForTest&
StrategySignalDecisionLogObserverSlotForTest() noexcept {
  static StrategySignalDecisionLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline StrategyOrderIntentLogObserverForTest&
StrategyOrderIntentLogObserverSlotForTest() noexcept {
  static StrategyOrderIntentLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline StrategyOrderIntentRejectedLogObserverForTest&
StrategyOrderIntentRejectedLogObserverSlotForTest() noexcept {
  static StrategyOrderIntentRejectedLogObserverForTest observer = nullptr;
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

[[nodiscard]] inline StrategySubmitStageObserverForTest&
StrategySubmitStageObserverSlotForTest() noexcept {
  static StrategySubmitStageObserverForTest observer = nullptr;
  return observer;
}

inline void SetStrategySignalTriggeredLogObserverForTest(
    StrategySignalTriggeredLogObserverForTest observer) noexcept {
  StrategySignalTriggeredLogObserverSlotForTest() = observer;
}

inline void SetStrategySignalDecisionLogObserverForTest(
    StrategySignalDecisionLogObserverForTest observer) noexcept {
  StrategySignalDecisionLogObserverSlotForTest() = observer;
}

inline void SetStrategyOrderIntentLogObserverForTest(
    StrategyOrderIntentLogObserverForTest observer) noexcept {
  StrategyOrderIntentLogObserverSlotForTest() = observer;
}

inline void SetStrategyOrderIntentRejectedLogObserverForTest(
    StrategyOrderIntentRejectedLogObserverForTest observer) noexcept {
  StrategyOrderIntentRejectedLogObserverSlotForTest() = observer;
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

inline void SetStrategySubmitStageObserverForTest(
    StrategySubmitStageObserverForTest observer) noexcept {
  StrategySubmitStageObserverSlotForTest() = observer;
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

inline void NotifyStrategySignalDecisionLogObserverForTest(
    const StrategySignalDecisionLogRecordForTest& record) noexcept {
  StrategySignalDecisionLogObserverForTest observer =
      StrategySignalDecisionLogObserverSlotForTest();
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

inline void NotifyStrategyOrderIntentRejectedLogObserverForTest(
    const StrategyOrderIntentRejectedLogRecordForTest& record) noexcept {
  StrategyOrderIntentRejectedLogObserverForTest observer =
      StrategyOrderIntentRejectedLogObserverSlotForTest();
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

inline void NotifyStrategySubmitStageObserverForTest(
    const StrategySubmitStageRecordForTest& record) noexcept {
  StrategySubmitStageObserverForTest observer =
      StrategySubmitStageObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(record);
}

}  // namespace detail
}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_STRATEGY_TEST_HOOKS_H_
