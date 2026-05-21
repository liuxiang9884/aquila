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

#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
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
    ApplyFinishedOrder(event.local_order_id, context);
  }

  template <typename ContextT>
  void OnOrderFeedback(const OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    if (event.kind == OrderFeedbackKind::kContinuityLost) {
      degraded_ = true;
      for (PairRuntimeState& runtime : pair_runtime_by_symbol_id_) {
        if (runtime.initialized) {
          runtime.execution.OnFeedbackContinuityLost(event);
        }
      }
      return;
    }
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
    return degraded_;
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
    std::size_t size{0};
    bool active{false};

    [[nodiscard]] std::string_view view() const noexcept {
      return std::string_view(price_text.data(), size);
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

    std::int64_t quantity = 0;
    ExecutionGroup* close_group = nullptr;
    if (last_signal_decision_.intent.reduce_only) {
      close_group =
          runtime->execution.FindGroupById(last_signal_decision_.group_id);
      if (close_group == nullptr) {
        return;
      }
      quantity = AbsolutePositionQuantity(*close_group);
    } else {
      quantity =
          OpenOrderQuantity(runtime->pair, last_signal_decision_.intent.price,
                            last_signal_decision_.intent.side);
    }
    if (quantity <= 0) {
      return;
    }

    const double order_price = RoundedOrderPrice(
        last_signal_decision_.intent.price, runtime->pair.lag_instrument,
        last_signal_decision_.intent.side);
    if (order_price <= 0.0) {
      return;
    }

    OrderPriceTextStorage* price_text_storage = AcquireOrderPriceText(
        order_price, runtime->pair.lag_instrument.price_decimal_places);
    if (price_text_storage == nullptr) {
      return;
    }
    const std::string_view price_text = price_text_storage->view();
    const std::string_view symbol =
        runtime->pair.lag_instrument.exchange_symbol.empty()
            ? std::string_view(runtime->pair.symbol)
            : std::string_view(runtime->pair.lag_instrument.exchange_symbol);

    const core::OrderPlaceResult placed =
        context.PlaceOrder(core::OrderCreateRequest{
            .exchange = last_signal_decision_.intent.exchange,
            .symbol_id = last_signal_decision_.intent.symbol_id,
            .symbol = symbol,
            .side = last_signal_decision_.intent.side,
            .order_type = OrderType::kLimit,
            .time_in_force = TimeInForce::kImmediateOrCancel,
            .quantity = quantity,
            .price_text = price_text,
            .reduce_only = last_signal_decision_.intent.reduce_only,
        });
    if (placed.local_order_id == 0) {
      ReleaseOrderPriceText(price_text_storage);
      return;
    }
    price_text_storage->local_order_id = placed.local_order_id;

    if (placed.status == core::OrderPlaceStatus::kOk) {
      OnExternalOrderAccepted(runtime, close_group, placed.local_order_id);
      return;
    }

    RollbackRejectedSubmit(runtime, close_group, placed.local_order_id,
                           context);
  }

  void OnExternalOrderAccepted(PairRuntimeState* runtime,
                               ExecutionGroup* close_group,
                               std::uint64_t local_order_id) noexcept {
    ExecutionGroup* group = close_group;
    if (last_signal_decision_.intent.reduce_only) {
      if (group == nullptr ||
          !runtime->execution.StartCloseOrder(*group, local_order_id)) {
        return;
      }
    } else {
      group = runtime->execution.StartOpenOrder(local_order_id);
      if (group == nullptr) {
        return;
      }
      last_signal_decision_.group_id = group->group_id;
    }
    UpdateSubmittedSignalDiagnostics(runtime, group);
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

  [[nodiscard]] static std::int64_t AbsolutePositionQuantity(
      const ExecutionGroup& group) noexcept {
    if (group.signed_position_quantity ==
        std::numeric_limits<std::int64_t>::min()) {
      return 0;
    }
    return group.signed_position_quantity >= 0
               ? group.signed_position_quantity
               : -group.signed_position_quantity;
  }

  [[nodiscard]] static std::int64_t OpenOrderQuantity(const PairConfig& pair,
                                                      double intent_price,
                                                      OrderSide side) noexcept {
    const InstrumentMetadata& instrument = pair.lag_instrument;
    const double order_price =
        RoundedOrderPrice(intent_price, instrument, side);
    if (order_price <= 0.0 || instrument.notional_multiplier <= 0.0 ||
        instrument.quantity_step <= 0.0 || pair.execute.open_notional <= 0.0) {
      return 0;
    }

    const double raw_quantity = pair.execute.open_notional /
                                (order_price * instrument.notional_multiplier);
    if (!std::isfinite(raw_quantity) || raw_quantity <= 0.0) {
      return 0;
    }

    double quantity = FloorToStep(raw_quantity, instrument.quantity_step);
    if (instrument.max_quantity > 0.0 && quantity > instrument.max_quantity) {
      quantity = FloorToStep(instrument.max_quantity, instrument.quantity_step);
    }
    if (instrument.min_quantity > 0.0 &&
        quantity + kQuantityEpsilon < instrument.min_quantity) {
      return 0;
    }
    if (!std::isfinite(quantity) || quantity <= 0.0 ||
        quantity >
            static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
      return 0;
    }
    return static_cast<std::int64_t>(quantity);
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

  [[nodiscard]] static double FloorToStep(double quantity,
                                          double step) noexcept {
    if (!std::isfinite(quantity) || !std::isfinite(step) || step <= 0.0) {
      return 0.0;
    }
    return std::floor(quantity / step + kQuantityEpsilon) * step;
  }

  [[nodiscard]] OrderPriceTextStorage* AcquireOrderPriceText(
      double price, std::int32_t decimal_places) noexcept {
    if (!ValidOrderPriceDecimalPlaces(decimal_places) ||
        !std::isfinite(price) || price <= 0.0) {
      return nullptr;
    }
    for (OrderPriceTextStorage& storage : order_price_texts_) {
      if (storage.active) {
        continue;
      }
      char* const begin = storage.price_text.data();
      char* const end = begin + storage.price_text.size();
      const auto result =
          std::to_chars(begin, end, price, std::chars_format::fixed,
                        static_cast<int>(decimal_places));
      if (result.ec != std::errc{} || result.ptr == begin) {
        storage.size = 0;
        storage.local_order_id = 0;
        storage.active = false;
        return nullptr;
      }
      storage.size = static_cast<std::size_t>(result.ptr - begin);
      storage.local_order_id = 0;
      storage.active = true;
      return &storage;
    }
    return nullptr;
  }

  static void ReleaseOrderPriceText(OrderPriceTextStorage* storage) noexcept {
    if (storage == nullptr) {
      return;
    }
    storage->local_order_id = 0;
    storage->size = 0;
    storage->active = false;
  }

  [[nodiscard]] static bool ValidOrderPriceDecimalPlaces(
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
  bool degraded_{false};
  bool stop_requested_{false};
  static constexpr double kPriceEpsilon = 1e-12;
  static constexpr double kQuantityEpsilon = 1e-12;
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
