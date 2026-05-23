#ifndef AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
#define AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
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

namespace detail {

#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
struct StrategyOrderIntentLogRecordForTest {
  std::string_view symbol;
  std::int32_t symbol_id{0};
  SignalAction action{SignalAction::kNone};
  OrderSide side{OrderSide::kBuy};
  bool reduce_only{false};
  std::uint64_t group_id{0};
  double quantity{0.0};
  double raw_price{0.0};
  double order_price{0.0};
  double price{0.0};
  double target_open_notional{0.0};
  double estimated_notional{0.0};
  std::size_t active_groups{0};
};

using StrategyOrderIntentLogObserverForTest =
    void (*)(const StrategyOrderIntentLogRecordForTest& record) noexcept;

[[nodiscard]] inline StrategyOrderIntentLogObserverForTest&
StrategyOrderIntentLogObserverSlotForTest() noexcept {
  static StrategyOrderIntentLogObserverForTest observer = nullptr;
  return observer;
}

inline void SetStrategyOrderIntentLogObserverForTest(
    StrategyOrderIntentLogObserverForTest observer) noexcept {
  StrategyOrderIntentLogObserverSlotForTest() = observer;
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
#endif

inline void LogStrategyOrderIntent(
    std::string_view symbol, std::int32_t symbol_id, SignalAction action,
    OrderSide side, bool reduce_only, std::uint64_t group_id,
    double quantity, double raw_price, double order_price,
    std::uint32_t slippage_ticks, double price_tick,
    double target_open_notional, double estimated_notional,
    std::size_t active_groups) noexcept {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_INFO(
        "lead_lag_order_intent symbol={} symbol_id={} action={} side={} "
        "reduce_only={} group_id={} quantity={:.12g} price={:.12g} "
        "raw_price={:.12g} order_price={:.12g} slippage_ticks={} "
        "price_tick={:.12g} target_open_notional={:.12g} "
        "estimated_notional={:.12g} active_groups={}",
        symbol, symbol_id, magic_enum::enum_name(action),
        magic_enum::enum_name(side), reduce_only ? "true" : "false", group_id,
        quantity, order_price, raw_price, order_price, slippage_ticks,
        price_tick, target_open_notional, estimated_notional, active_groups);
  }
#if defined(AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS)
  NotifyStrategyOrderIntentLogObserverForTest(
      StrategyOrderIntentLogRecordForTest{
          .symbol = symbol,
          .symbol_id = symbol_id,
          .action = action,
          .side = side,
          .reduce_only = reduce_only,
          .group_id = group_id,
          .quantity = quantity,
          .raw_price = raw_price,
          .order_price = order_price,
          .price = order_price,
          .target_open_notional = target_open_notional,
          .estimated_notional = estimated_notional,
          .active_groups = active_groups});
#endif
}

inline void LogStrategyOrderIntentRejected(
    std::string_view reason, std::string_view symbol, std::int32_t symbol_id,
    SignalAction action, OrderSide side, bool reduce_only,
    std::uint64_t group_id, double quantity, double raw_price,
    double order_price, std::uint32_t slippage_ticks, double price_tick,
    double target_open_notional, double estimated_notional,
    double gross_before = 0.0, double gross_after = 0.0,
    double max_gross_notional = 0.0, std::uint64_t local_order_id = 0,
    std::string_view place_status = "-") noexcept {
  if (::nova::kLogManager.logger() == nullptr) {
    return;
  }
  NOVA_WARNING(
      "lead_lag_order_intent_rejected reason={} symbol={} symbol_id={} "
      "action={} side={} reduce_only={} group_id={} quantity={:.12g} "
      "price={:.12g} raw_price={:.12g} order_price={:.12g} "
      "slippage_ticks={} price_tick={:.12g} target_open_notional={:.12g} "
      "estimated_notional={:.12g} gross_before={:.12g} gross_after={:.12g} "
      "max_gross_notional={:.12g} local_order_id={} place_status={}",
      reason, symbol, symbol_id, magic_enum::enum_name(action),
      magic_enum::enum_name(side), reduce_only ? "true" : "false", group_id,
      quantity, order_price, raw_price, order_price, slippage_ticks, price_tick,
      target_open_notional, estimated_notional, gross_before, gross_after,
      max_gross_notional, local_order_id, place_status);
}

inline void LogStrategyOrderResponse(
    const core::OrderResponseEvent& event) noexcept {
  if (::nova::kLogManager.logger() == nullptr) {
    return;
  }
  NOVA_INFO(
      "lead_lag_order_response kind={} local_order_id={} "
      "exchange_order_id={}",
      magic_enum::enum_name(event.kind), event.local_order_id,
      event.exchange_order_id);
}

inline void LogStrategyOrderFeedback(const OrderFeedbackEvent& event) noexcept {
  if (::nova::kLogManager.logger() == nullptr) {
    return;
  }
  NOVA_INFO(
      "lead_lag_order_feedback kind={} local_order_id={} exchange_order_id={} "
      "cumulative_filled_quantity={:.12g} left_quantity={:.12g} "
      "cancelled_quantity={:.12g} "
      "fill_price={:.12g} role={} finish_reason={} reject_reason={}",
      magic_enum::enum_name(event.kind), event.local_order_id,
      event.exchange_order_id, event.cumulative_filled_quantity,
      event.left_quantity, event.cancelled_quantity, event.fill_price,
      magic_enum::enum_name(event.role),
      magic_enum::enum_name(event.finish_reason),
      magic_enum::enum_name(event.reject_reason));
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

inline void LogStrategyOrderFinished(const core::StrategyOrder& order,
                                     std::size_t active_groups) noexcept {
  if (::nova::kLogManager.logger() == nullptr) {
    return;
  }
  NOVA_INFO(
      "lead_lag_order_finished local_order_id={} symbol_id={} symbol={} "
      "status={} reduce_only={} quantity={:.12g} "
      "cumulative_filled_quantity={:.12g} "
      "average_fill_price={:.12g} last_fill_price={:.12g} "
      "exchange_order_id={} active_groups={}",
      order.local_order_id, order.symbol_id, order.symbol,
      magic_enum::enum_name(order.status), order.reduce_only ? "true" : "false",
      order.quantity, order.cumulative_filled_quantity,
      order.AverageFillPrice(), order.last_fill_price, order.exchange_order_id,
      active_groups);
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
    last_signal_decision_ = {};
    last_signal_diagnostics_valid_ = false;
    last_market_update_ = raw_market_state_.OnBookTicker(ticker);
    if (!last_market_update_.tracked) {
      return;
    }

    PairRuntimeState* runtime = MutableRuntime(last_market_update_.symbol_id);
    const PairMarketState* market =
        raw_market_state_.FindPair(last_market_update_.symbol_id);
    if (runtime == nullptr || market == nullptr) {
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
        OnActiveLeadTick(runtime, *market, context);
        break;
      case PairRole::kLag:
        OnActiveLagTick(runtime, *market, context);
        break;
      case PairRole::kNone:
        break;
    }
  }

  template <typename ContextT>
  void OnOrderResponse(const core::OrderResponseEvent& event,
                       ContextT& context) noexcept {
    detail::LogStrategyOrderResponse(event);
    ApplyFinishedOrder(event.local_order_id, context);
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
    detail::LogStrategyOrderFeedback(event);
    ApplyFinishedOrder(event.local_order_id, context);
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
  static constexpr std::int32_t kMaxOrderPriceDecimalPlaces = 18;

  struct PairRuntimeState {
    bool initialized{false};
    PairConfig pair;
    AlignmentState alignment;
    RecorderState recorder;
    ThresholdState threshold;
    ExecutionState execution;
    QuoteSnapshot drifted_lead;
    bool has_drifted_lead{false};
  };

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
    order_price_texts_.clear();
    pair_runtime_by_symbol_id_.resize(
        static_cast<std::size_t>(max_symbol_id + 1));
    std::size_t price_text_slot_count = 0;
    for (const PairConfig& pair : config_.pairs) {
      if (!RuntimeConfigReady(pair)) {
        continue;
      }
      price_text_slot_count += pair.execute.parallel;
      PairRuntimeState& runtime =
          pair_runtime_by_symbol_id_[static_cast<std::size_t>(pair.symbol_id)];
      runtime.initialized = true;
      runtime.pair = pair;
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
      last_signal_diagnostics_ = BuildSignalDiagnostics(
          *runtime, market, drifted_lead, recorder, alignment, threshold);
      last_signal_diagnostics_valid_ = true;
    }
    if (SyntheticPositionAccounting()) {
      ApplySyntheticSignal(runtime, last_signal_decision_);
    } else {
      SubmitExternalSignal(runtime, context);
    }
  }

  template <typename ContextT>
  void OnActiveLagTick(PairRuntimeState* runtime, const PairMarketState& market,
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
      last_signal_diagnostics_ =
          BuildSignalDiagnostics(*runtime, market, runtime->drifted_lead,
                                 recorder, alignment, threshold);
      last_signal_diagnostics_valid_ = true;
    }
    if (SyntheticPositionAccounting()) {
      ApplySyntheticSignal(runtime, last_signal_decision_);
    } else {
      SubmitExternalSignal(runtime, context);
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
                            ContextT& context) noexcept {
    if (runtime == nullptr || !last_signal_decision_.triggered) {
      return;
    }

    const InstrumentMetadata& instrument = runtime->pair.lag_instrument;
    const double raw_order_price = last_signal_decision_.intent.price;
    const std::uint32_t slippage_ticks = SlippageTicksForAction(
        runtime->pair.execute, last_signal_decision_.action);
    const double order_price = SlippedRoundedOrderPrice(
        raw_order_price, instrument, last_signal_decision_.intent.side,
        slippage_ticks);
    const std::string_view symbol =
        instrument.exchange_symbol.empty()
            ? std::string_view(runtime->pair.symbol)
            : std::string_view(instrument.exchange_symbol);
    if (order_price <= 0.0) {
      detail::LogStrategyOrderIntentRejected(
          "invalid_price", symbol, runtime->pair.symbol_id,
          last_signal_decision_.action, last_signal_decision_.intent.side,
          last_signal_decision_.intent.reduce_only,
          last_signal_decision_.group_id, 0, raw_order_price, order_price,
          slippage_ticks, instrument.price_tick,
          runtime->pair.execute.open_notional, 0.0);
      return;
    }

    double quantity = 0.0;
    ExecutionGroup* close_group = nullptr;
    if (last_signal_decision_.intent.reduce_only) {
      close_group =
          runtime->execution.FindGroupById(last_signal_decision_.group_id);
      if (close_group == nullptr) {
        return;
      }
      quantity = AbsolutePositionQuantity(*close_group);
    } else {
      quantity = OpenOrderQuantity(runtime->pair, order_price);
    }
    if (quantity <= kQuantityEpsilon) {
      detail::LogStrategyOrderIntentRejected(
          "zero_quantity", symbol, runtime->pair.symbol_id,
          last_signal_decision_.action, last_signal_decision_.intent.side,
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
          "risk_limit", symbol, runtime->pair.symbol_id,
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

    OrderPriceTextStorage* order_text_storage = AcquireOrderText(
        order_price, instrument.price_decimal_places, quantity,
        instrument.quantity_decimal_places);
    if (order_text_storage == nullptr) {
      detail::LogStrategyOrderIntentRejected(
          "order_text_slot_full", symbol, runtime->pair.symbol_id,
          last_signal_decision_.action, last_signal_decision_.intent.side,
          last_signal_decision_.intent.reduce_only,
          last_signal_decision_.group_id, quantity, raw_order_price,
          order_price, slippage_ticks, instrument.price_tick,
          runtime->pair.execute.open_notional, order_notional);
      return;
    }
    const std::string_view quantity_text = order_text_storage->quantity_view();
    const std::string_view price_text = order_text_storage->price_view();

    detail::LogStrategyOrderIntent(
        symbol, runtime->pair.symbol_id, last_signal_decision_.action,
        last_signal_decision_.intent.side,
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
          "place_local_rejected", symbol, runtime->pair.symbol_id,
          last_signal_decision_.action, last_signal_decision_.intent.side,
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
      if (tracked && !last_signal_decision_.intent.reduce_only) {
        ReserveOpenRisk(order_text_storage, quantity, order_notional);
      }
      return;
    }

    detail::LogStrategyOrderIntentRejected(
        "place_local_rejected", symbol, runtime->pair.symbol_id,
        last_signal_decision_.action, last_signal_decision_.intent.side,
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
  void ApplyFinishedOrder(std::uint64_t local_order_id,
                          ContextT& context) noexcept {
    if (local_order_id == 0) {
      return;
    }
    const core::StrategyOrder* order = context.FindOrder(local_order_id);
    if (order == nullptr || !order->is_finished) {
      return;
    }
    PairRuntimeState* runtime = MutableRuntime(order->symbol_id);
    if (runtime != nullptr) {
      [[maybe_unused]] const ExecutionApplyResult applied =
          runtime->execution.ApplyTerminalOrder(*order,
                                                runtime->pair.lag_instrument);
      detail::LogStrategyOrderFinished(*order,
                                       runtime->execution.active_group_count());
    } else {
      detail::LogStrategyOrderFinished(*order, 0);
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

  [[nodiscard]] static double OpenOrderQuantity(
      const PairConfig& pair, double order_price) noexcept {
    const InstrumentMetadata& instrument = pair.lag_instrument;
    if (order_price <= 0.0 || instrument.notional_multiplier <= 0.0 ||
        instrument.quantity_step <= 0.0 || pair.execute.open_notional <= 0.0) {
      return 0.0;
    }

    const double raw_quantity = pair.execute.open_notional /
                                (order_price * instrument.notional_multiplier);
    if (!std::isfinite(raw_quantity) || raw_quantity <= 0.0) {
      return 0.0;
    }

    double quantity = FloorToStep(raw_quantity, instrument.quantity_step);
    if (instrument.max_quantity > 0.0 && quantity > instrument.max_quantity) {
      quantity = FloorToStep(instrument.max_quantity, instrument.quantity_step);
    }
    if (instrument.min_quantity > 0.0 &&
        quantity + kQuantityEpsilon < instrument.min_quantity) {
      return 0.0;
    }
    if (!std::isfinite(quantity) || quantity <= kQuantityEpsilon) {
      return 0.0;
    }
    return quantity;
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
      if (quantity >
          config_.risk.max_holding_position - totals.holding_position +
              kQuantityEpsilon) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static double OrderNotional(
      double quantity, double price,
      const InstrumentMetadata& instrument) noexcept {
    if (quantity <= kQuantityEpsilon || !std::isfinite(price) ||
        price <= 0.0 ||
        !std::isfinite(instrument.notional_multiplier) ||
        instrument.notional_multiplier <= 0.0) {
      return 0.0;
    }
    return quantity * price * instrument.notional_multiplier;
  }

  [[nodiscard]] static double RoundedOrderPrice(
      double intent_price, const InstrumentMetadata& instrument,
      OrderSide side) noexcept {
    if (!std::isfinite(intent_price) || intent_price <= 0.0 ||
        instrument.price_tick <= 0.0 || instrument.price_decimal_places < 0) {
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
    if (!std::isfinite(raw_price) || raw_price <= 0.0 ||
        !std::isfinite(instrument.price_tick) || instrument.price_tick <= 0.0) {
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

  [[nodiscard]] static double FloorToStep(double quantity,
                                          double step) noexcept {
    if (!std::isfinite(quantity) || !std::isfinite(step) || step <= 0.0) {
      return 0.0;
    }
    return std::floor(quantity / step + kQuantityEpsilon) * step;
  }

  [[nodiscard]] OrderPriceTextStorage* AcquireOrderText(
      double price, std::int32_t price_decimal_places, double quantity,
      std::int32_t quantity_decimal_places) noexcept {
    if (!ValidOrderDecimalPlaces(price_decimal_places) ||
        !ValidOrderDecimalPlaces(quantity_decimal_places) ||
        !std::isfinite(price) || price <= 0.0 || !std::isfinite(quantity) ||
        quantity <= 0.0) {
      return nullptr;
    }
    for (OrderPriceTextStorage& storage : order_price_texts_) {
      if (storage.active) {
        continue;
      }
      char* const price_begin = storage.price_text.data();
      char* const price_end = price_begin + storage.price_text.size();
      const auto price_result =
          std::to_chars(price_begin, price_end, price, std::chars_format::fixed,
                        static_cast<int>(price_decimal_places));
      if (price_result.ec != std::errc{} || price_result.ptr == price_begin) {
        ReleaseOrderPriceText(&storage);
        return nullptr;
      }
      char* const quantity_begin = storage.quantity_text.data();
      char* const quantity_end =
          quantity_begin + storage.quantity_text.size();
      const auto quantity_result =
          std::to_chars(quantity_begin, quantity_end, quantity,
                        std::chars_format::fixed,
                        static_cast<int>(quantity_decimal_places));
      if (quantity_result.ec != std::errc{} ||
          quantity_result.ptr == quantity_begin) {
        ReleaseOrderPriceText(&storage);
        return nullptr;
      }
      storage.price_size =
          static_cast<std::size_t>(price_result.ptr - price_begin);
      storage.quantity_size =
          static_cast<std::size_t>(quantity_result.ptr - quantity_begin);
      storage.local_order_id = 0;
      storage.reserved_open_quantity = 0.0;
      storage.reserved_open_notional = 0.0;
      storage.active = true;
      return &storage;
    }
    return nullptr;
  }

  static void ReserveOpenRisk(OrderPriceTextStorage* storage,
                              double quantity, double notional) noexcept {
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

  [[nodiscard]] static bool ValidOrderDecimalPlaces(
      std::int32_t decimal_places) noexcept {
    return decimal_places >= 0 && decimal_places <= kMaxOrderPriceDecimalPlaces;
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

  Config config_;
  StrategyOptions options_;
  RawMarketState raw_market_state_;
  std::vector<PairRuntimeState> pair_runtime_by_symbol_id_;
  std::vector<OrderPriceTextStorage> order_price_texts_;
  MarketUpdate last_market_update_;
  SignalDecision last_signal_decision_;
  SignalDiagnostics last_signal_diagnostics_;
  bool last_signal_diagnostics_valid_{false};
  RecoveryState recovery_state_{RecoveryState::kNormal};
  bool stop_requested_{false};
  static constexpr double kPriceEpsilon = 1e-12;
  static constexpr double kQuantityEpsilon = 1e-12;
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
