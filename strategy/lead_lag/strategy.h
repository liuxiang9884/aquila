#ifndef AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
#define AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/market_data/types.h"
#include "core/strategy/order_types.h"
#include "core/trading/order_feedback_event.h"
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
  PositionDirection position_direction{PositionDirection::kNone};
  double trailing_price{0.0};
};

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
  void OnBookTicker(const BookTicker& ticker, ContextT&) noexcept {
    last_signal_decision_ = {};
    last_signal_diagnostics_ = {};
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
        OnActiveLeadTick(runtime, *market);
        break;
      case PairRole::kLag:
        OnActiveLagTick(runtime, *market);
        break;
      case PairRole::kNone:
        break;
    }
  }

  template <typename ContextT>
  void OnOrderResponse(const strategy::OrderResponseEvent&,
                       ContextT&) noexcept {}

  template <typename ContextT>
  void OnOrderFeedback(const OrderFeedbackEvent& event, ContextT&) noexcept {
    if (event.kind == OrderFeedbackKind::kGap) {
      degraded_ = true;
      for (PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
        if (runtime.initialized) {
          runtime.execution.OnFeedbackGap(event);
        }
      }
    }
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
    return degraded_;
  }

  [[nodiscard]] const SignalDecision& last_signal_decision() const noexcept {
    return last_signal_decision_;
  }

  [[nodiscard]] const SignalDiagnostics& last_signal_diagnostics()
      const noexcept {
    return last_signal_diagnostics_;
  }

 private:
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
    pair_runtime_by_symbol_id_.resize(
        static_cast<std::size_t>(max_symbol_id + 1));
    for (const PairConfig& pair : config_.pairs) {
      if (!RuntimeConfigReady(pair)) {
        continue;
      }
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

  void OnActiveLeadTick(PairRuntimeState* runtime,
                        const PairMarketState& market) noexcept {
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
    }
    if (SyntheticPositionAccounting()) {
      ApplySyntheticSignal(runtime, last_signal_decision_);
    }
  }

  void OnActiveLagTick(PairRuntimeState* runtime,
                       const PairMarketState& market) noexcept {
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
      last_signal_diagnostics_ = BuildSignalDiagnostics(
          *runtime, market, runtime->drifted_lead, recorder, alignment,
          threshold);
    }
    if (SyntheticPositionAccounting()) {
      ApplySyntheticSignal(runtime, last_signal_decision_);
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
            runtime->execution.AddHoldGroup(/*signed_position_quantity=*/1,
                                            decision.intent.price);
        break;
      }
      case SignalAction::kOpenShort: {
        [[maybe_unused]] ExecutionGroup* short_group =
            runtime->execution.AddHoldGroup(/*signed_position_quantity=*/-1,
                                            decision.intent.price);
        break;
      }
      case SignalAction::kCloseLong:
      case SignalAction::kStoplossLong:
        ClearFirstSyntheticHold(runtime, /*long_position=*/true);
        break;
      case SignalAction::kCloseShort:
      case SignalAction::kStoplossShort:
        ClearFirstSyntheticHold(runtime, /*long_position=*/false);
        break;
      case SignalAction::kNone:
        break;
    }
  }

  static void ClearFirstSyntheticHold(PairRuntimeState* runtime,
                                      bool long_position) noexcept {
    for (ExecutionGroup& group : runtime->execution.groups()) {
      if (!group.hold()) {
        continue;
      }
      if ((long_position && group.long_position()) ||
          (!long_position && group.short_position())) {
        group = ExecutionGroup{};
        return;
      }
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
        .position_direction =
            PositionDirectionForAction(last_signal_decision_.action),
        .trailing_price =
            TrailingPriceForAction(runtime.execution, last_signal_decision_),
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

  [[nodiscard]] static double TrailingPriceForAction(
      const ExecutionState& execution,
      const SignalDecision& decision) noexcept {
    const PositionDirection direction =
        PositionDirectionForAction(decision.action);
    if (direction == PositionDirection::kNone) {
      return 0.0;
    }
    for (const ExecutionGroup& group : execution.groups()) {
      if (!group.hold()) {
        continue;
      }
      if ((direction == PositionDirection::kLong && group.long_position()) ||
          (direction == PositionDirection::kShort && group.short_position())) {
        return group.trailing_price;
      }
    }
    return 0.0;
  }

  Config config_;
  StrategyOptions options_;
  RawMarketState raw_market_state_;
  std::vector<PairRuntimeState> pair_runtime_by_symbol_id_;
  MarketUpdate last_market_update_;
  SignalDecision last_signal_decision_;
  SignalDiagnostics last_signal_diagnostics_;
  bool degraded_{false};
  bool stop_requested_{false};
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
