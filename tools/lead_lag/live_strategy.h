#ifndef AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_
#define AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "core/config/strategy_config.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/execution_state.h"
#include "strategy/lead_lag/strategy.h"

namespace aquila::tools::lead_lag {

inline constexpr int kContinuityLostEmergencyHandoffExitCode = 10;

enum class RunMode : std::uint8_t {
  kValidateOnly,
  kSignalOnly,
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
  kMarketCalc,
#endif
  kLiveOrders,
  kLiveOpenCloseSmoke,
  kLiveUnfilledCancelSmoke,
  kLiveSubmitRejectSmoke,
};

struct SmokeModeSelection {
  bool open_close{false};
  bool unfilled_cancel{false};
  bool submit_reject{false};
};

struct RunModeResult {
  bool ok{false};
  RunMode mode{RunMode::kValidateOnly};
  std::string error;
};

struct RecoveryDiagnostics {
  strategy::leadlag::RecoveryState recovery_state{
      strategy::leadlag::RecoveryState::kNormal};
  bool needs_reconcile{false};
  bool manual_intervention{false};
  bool new_entries_paused{false};
};

struct LiveOrdersStrategyStats {
  std::uint64_t book_tickers{0};
  std::uint64_t order_responses{0};
  std::uint64_t order_feedbacks{0};
  bool continuity_lost_stop_requested{false};
  RecoveryDiagnostics recovery;
};

enum class LiveOpenCloseSmokeState : std::uint8_t {
  kWaitingTicker,
  kOpenPending,
  kWaitingCloseTicker,
  kClosePending,
  kDone,
  kError,
};

struct LiveOpenCloseSmokeOptions {
  std::string symbol;
  double aggressive_price_bps{100.0};
  double max_notional{100.0};
};

struct LiveOpenCloseSmokeStats {
  std::uint64_t book_tickers{0};
  std::uint64_t order_responses{0};
  std::uint64_t order_feedbacks{0};
  LiveOpenCloseSmokeState state{LiveOpenCloseSmokeState::kWaitingTicker};
  std::uint64_t open_local_order_id{0};
  std::uint64_t close_local_order_id{0};
  double open_quantity{0.0};
  double close_quantity{0.0};
  double target_notional{0.0};
  double estimated_open_notional{0.0};
  bool used_min_quantity{false};
  bool completed{false};
  bool continuity_lost_stop_requested{false};
  std::string error;
};

enum class LiveUnfilledCancelSmokeState : std::uint8_t {
  kWaitingTicker,
  kOpenPending,
  kCancelPending,
  kDone,
  kError,
};

struct LiveUnfilledCancelSmokeOptions {
  std::string symbol;
  double passive_price_bps{200.0};
  double max_notional{100.0};
};

struct LiveUnfilledCancelSmokeStats {
  std::uint64_t book_tickers{0};
  std::uint64_t order_responses{0};
  std::uint64_t order_feedbacks{0};
  LiveUnfilledCancelSmokeState state{
      LiveUnfilledCancelSmokeState::kWaitingTicker};
  std::uint64_t open_local_order_id{0};
  double open_quantity{0.0};
  double target_notional{0.0};
  double estimated_open_notional{0.0};
  bool used_min_quantity{false};
  bool cancel_requested{false};
  bool completed{false};
  bool continuity_lost_stop_requested{false};
  std::string error;
};

enum class LiveSubmitRejectSmokeState : std::uint8_t {
  kWaitingTicker,
  kOrderPending,
  kDone,
  kError,
};

struct LiveSubmitRejectSmokeOptions {
  std::string symbol;
  double max_notional{100.0};
};

struct LiveSubmitRejectSmokeStats {
  std::uint64_t book_tickers{0};
  std::uint64_t order_responses{0};
  std::uint64_t order_feedbacks{0};
  LiveSubmitRejectSmokeState state{LiveSubmitRejectSmokeState::kWaitingTicker};
  std::uint64_t local_order_id{0};
  double quantity{0.0};
  double target_notional{0.0};
  double estimated_notional{0.0};
  bool used_min_quantity{false};
  bool rejected_seen{false};
  bool completed{false};
  bool continuity_lost_stop_requested{false};
  std::string error;
};

namespace detail {

inline constexpr std::int32_t kSmokeOrderDecimalPlaceLimit = 12;

enum class SmokeOrderMetadataUse : std::uint8_t {
  kNotionalSizedOrder,
  kMinimumQuantityOrder,
};

[[nodiscard]] inline bool SmokeOrderDecimalPlacesWithinRuntimeBounds(
    const strategy::leadlag::InstrumentMetadata& instrument) noexcept {
  const std::int32_t price_decimal_places = instrument.price_decimal_places;
  const std::int32_t quantity_decimal_places =
      instrument.quantity_decimal_places;
  return price_decimal_places >= 0 &&
         price_decimal_places < kSmokeOrderDecimalPlaceLimit &&
         quantity_decimal_places >= 0 &&
         quantity_decimal_places < kSmokeOrderDecimalPlaceLimit &&
         price_decimal_places + quantity_decimal_places <
             kSmokeOrderDecimalPlaceLimit;
}

[[nodiscard]] inline bool SmokeOrderMetadataValid(
    const strategy::leadlag::PairConfig& pair,
    SmokeOrderMetadataUse use) noexcept {
  const strategy::leadlag::InstrumentMetadata& instrument = pair.lag_instrument;
  if (!SmokeOrderDecimalPlacesWithinRuntimeBounds(instrument)) {
    return false;
  }
  if (!std::isfinite(instrument.price_tick) || instrument.price_tick <= 0.0 ||
      !std::isfinite(instrument.notional_multiplier) ||
      instrument.notional_multiplier <= 0.0 ||
      !std::isfinite(instrument.quantity_step) ||
      instrument.quantity_step <= 0.0 ||
      !std::isfinite(instrument.min_quantity) ||
      instrument.min_quantity < 0.0 ||
      !std::isfinite(instrument.max_quantity) ||
      instrument.max_quantity < 0.0) {
    return false;
  }
  switch (use) {
    case SmokeOrderMetadataUse::kNotionalSizedOrder:
      return std::isfinite(pair.execute.open_notional) &&
             pair.execute.open_notional > 0.0;
    case SmokeOrderMetadataUse::kMinimumQuantityOrder:
      return instrument.min_quantity > 0.0;
  }
  return false;
}

}  // namespace detail

class LiveOrdersStrategy {
 public:
  explicit LiveOrdersStrategy(strategy::leadlag::Config config)
      : inner_(std::move(config)) {
    RefreshRecoveryDiagnostics();
  }

  LiveOrdersStrategy(strategy::leadlag::Config config,
                     LiveOrdersStrategyStats* stats)
      : inner_(std::move(config)),
        stats_(stats == nullptr ? &owned_stats_ : stats) {
    RefreshRecoveryDiagnostics();
  }

  template <typename ContextT>
  void OnBookTicker(const BookTicker& ticker, ContextT& context) noexcept {
    ++stats_->book_tickers;
    inner_.OnBookTicker(ticker, context);
  }

  template <typename ContextT>
  void OnOrderResponse(const core::OrderResponseEvent& event,
                       ContextT& context) noexcept {
    ++stats_->order_responses;
    inner_.OnOrderResponse(event, context);
    RefreshRecoveryDiagnostics();
  }

  template <typename ContextT>
  void OnOrderFeedback(const OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    ++stats_->order_feedbacks;
    inner_.OnOrderFeedback(event, context);
    if (event.kind == OrderFeedbackKind::kContinuityLost) {
      stats_->continuity_lost_stop_requested = true;
      inner_.RequestStop();
    }
    RefreshRecoveryDiagnostics();
  }

  [[nodiscard]] bool ShouldStop() const noexcept {
    return inner_.ShouldStop() || stats_->continuity_lost_stop_requested;
  }

  [[nodiscard]] const LiveOrdersStrategyStats& stats() const noexcept {
    return *stats_;
  }

 private:
  void RefreshRecoveryDiagnostics() noexcept {
    stats_->recovery = RecoveryDiagnostics{
        .recovery_state = inner_.recovery_state(),
        .needs_reconcile = inner_.needs_reconcile(),
        .manual_intervention = inner_.manual_intervention(),
        .new_entries_paused = inner_.new_entries_paused(),
    };
  }

  strategy::leadlag::Strategy inner_;
  LiveOrdersStrategyStats owned_stats_;
  LiveOrdersStrategyStats* stats_{&owned_stats_};
};

class LiveOpenCloseSmokeStrategy {
 public:
  LiveOpenCloseSmokeStrategy(strategy::leadlag::Config config,
                             LiveOpenCloseSmokeOptions options,
                             LiveOpenCloseSmokeStats* stats)
      : config_(std::move(config)),
        options_(std::move(options)),
        stats_(stats == nullptr ? &owned_stats_ : stats) {
    pair_ = FindSmokePair();
    if (pair_ == nullptr) {
      SetError("smoke symbol not found in lead_lag config");
      return;
    }
    ValidateOptions();
    if (!detail::SmokeOrderMetadataValid(
            *pair_, detail::SmokeOrderMetadataUse::kNotionalSizedOrder)) {
      SetError("invalid smoke instrument sizing metadata");
    }
  }

  template <typename ContextT>
  void OnBookTicker(const BookTicker& ticker, ContextT& context) noexcept {
    ++stats_->book_tickers;
    if (pair_ == nullptr) {
      return;
    }
    if (ticker.exchange != Exchange::kGate ||
        ticker.symbol_id != pair_->symbol_id) {
      return;
    }
    if (stats_->state == LiveOpenCloseSmokeState::kWaitingTicker) {
      SubmitOpen(ticker, context);
      return;
    }
    if (stats_->state == LiveOpenCloseSmokeState::kWaitingCloseTicker) {
      SubmitClose(ticker, context);
    }
  }

  template <typename ContextT>
  void OnOrderResponse(const core::OrderResponseEvent& event,
                       ContextT&) noexcept {
    ++stats_->order_responses;
    if ((event.local_order_id == stats_->open_local_order_id ||
         event.local_order_id == stats_->close_local_order_id) &&
        event.kind == core::OrderResponseKind::kRejected) {
      SetError("smoke order rejected by order session");
    }
  }

  template <typename ContextT>
  void OnOrderFeedback(const OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    ++stats_->order_feedbacks;
    if (event.kind == OrderFeedbackKind::kContinuityLost) {
      stats_->continuity_lost_stop_requested = true;
      SetError("feedback continuity lost");
      return;
    }
    if (event.local_order_id == stats_->open_local_order_id) {
      OnOpenFeedback(event, context);
      return;
    }
    if (event.local_order_id == stats_->close_local_order_id) {
      OnCloseFeedback(event);
    }
  }

  [[nodiscard]] bool ShouldStop() const noexcept {
    return stats_->state == LiveOpenCloseSmokeState::kDone ||
           stats_->state == LiveOpenCloseSmokeState::kError ||
           stats_->continuity_lost_stop_requested;
  }

  [[nodiscard]] const LiveOpenCloseSmokeStats& stats() const noexcept {
    return *stats_;
  }

 private:
  static constexpr double kEpsilon = 1e-9;

  const strategy::leadlag::PairConfig* FindSmokePair() const noexcept {
    if (options_.symbol.empty()) {
      return config_.pairs.empty() ? nullptr : &config_.pairs.front();
    }
    for (const strategy::leadlag::PairConfig& pair : config_.pairs) {
      if (pair.symbol == options_.symbol ||
          pair.lag_instrument.exchange_symbol == options_.symbol) {
        return &pair;
      }
    }
    return nullptr;
  }

  void ValidateOptions() noexcept {
    if (!std::isfinite(options_.aggressive_price_bps) ||
        options_.aggressive_price_bps < 0.0) {
      SetError("smoke aggressive price bps must be non-negative");
      return;
    }
    if (!std::isfinite(options_.max_notional) || options_.max_notional <= 0.0) {
      SetError("smoke max notional must be positive");
    }
  }

  template <typename ContextT>
  void SubmitOpen(const BookTicker& ticker, ContextT& context) noexcept {
    const double quantity = ComputeOpenQuantity(ticker.ask_price);
    if (quantity <= kEpsilon) {
      return;
    }
    const double order_price = RoundedPrice(
        ticker.ask_price * (1.0 + options_.aggressive_price_bps / 10000.0),
        OrderSide::kBuy);
    if (order_price <= 0.0) {
      SetError("invalid smoke open price");
      return;
    }

    open_price_text_ =
        FormatPrice(order_price, pair_->lag_instrument.price_decimal_places);
    open_quantity_text_ =
        FormatPrice(quantity, pair_->lag_instrument.quantity_decimal_places);
    const std::string_view symbol = GateSymbol();
    const core::OrderPlaceResult placed =
        context.PlaceOrder(core::OrderCreateRequest{
            .exchange = Exchange::kGate,
            .symbol_id = pair_->symbol_id,
            .symbol = symbol,
            .side = OrderSide::kBuy,
            .order_type = OrderType::kLimit,
            .time_in_force = TimeInForce::kImmediateOrCancel,
            .quantity = quantity,
            .quantity_text = open_quantity_text_,
            .price_text = open_price_text_,
            .reduce_only = false,
        });
    if (placed.status != core::OrderPlaceStatus::kOk ||
        placed.local_order_id == 0) {
      SetError("failed to place smoke open order");
      return;
    }

    stats_->open_local_order_id = placed.local_order_id;
    stats_->open_quantity = quantity;
    stats_->state = LiveOpenCloseSmokeState::kOpenPending;
  }

  template <typename ContextT>
  void OnOpenFeedback(const OrderFeedbackEvent& event,
                      ContextT& context) noexcept {
    switch (event.kind) {
      case OrderFeedbackKind::kFilled:
        PrepareClose(event.cumulative_filled_quantity);
        return;
      case OrderFeedbackKind::kCancelled:
        if (event.cumulative_filled_quantity > kEpsilon) {
          PrepareClose(event.cumulative_filled_quantity);
          return;
        }
        SetError("smoke open order did not fill");
        return;
      case OrderFeedbackKind::kRejected:
        SetError("smoke open order rejected");
        return;
      case OrderFeedbackKind::kAccepted:
      case OrderFeedbackKind::kPartialFilled:
      case OrderFeedbackKind::kContinuityLost:
        return;
    }
  }

  void PrepareClose(double filled_quantity) noexcept {
    if (stats_->state == LiveOpenCloseSmokeState::kClosePending ||
        stats_->state == LiveOpenCloseSmokeState::kDone) {
      return;
    }
    if (filled_quantity <= kEpsilon) {
      SetError("invalid smoke open filled quantity");
      return;
    }
    pending_close_quantity_ = filled_quantity;
    stats_->close_quantity = filled_quantity;
    stats_->state = LiveOpenCloseSmokeState::kWaitingCloseTicker;
  }

  template <typename ContextT>
  void SubmitClose(const BookTicker& ticker, ContextT& context) noexcept {
    if (pending_close_quantity_ <= kEpsilon) {
      SetError("invalid smoke close quantity");
      return;
    }
    const double order_price = RoundedPrice(
        ticker.bid_price * (1.0 - options_.aggressive_price_bps / 10000.0),
        OrderSide::kSell);
    if (order_price <= 0.0) {
      SetError("invalid smoke close price");
      return;
    }
    close_price_text_ =
        FormatPrice(order_price, pair_->lag_instrument.price_decimal_places);
    close_quantity_text_ = FormatPrice(
        pending_close_quantity_, pair_->lag_instrument.quantity_decimal_places);
    const core::OrderPlaceResult placed =
        context.PlaceOrder(core::OrderCreateRequest{
            .exchange = Exchange::kGate,
            .symbol_id = pair_->symbol_id,
            .symbol = GateSymbol(),
            .side = OrderSide::kSell,
            .order_type = OrderType::kLimit,
            .time_in_force = TimeInForce::kImmediateOrCancel,
            .quantity = pending_close_quantity_,
            .quantity_text = close_quantity_text_,
            .price_text = close_price_text_,
            .reduce_only = true,
        });
    if (placed.status != core::OrderPlaceStatus::kOk ||
        placed.local_order_id == 0) {
      SetError("failed to place smoke close order");
      return;
    }
    stats_->close_local_order_id = placed.local_order_id;
    stats_->state = LiveOpenCloseSmokeState::kClosePending;
  }

  void OnCloseFeedback(const OrderFeedbackEvent& event) noexcept {
    switch (event.kind) {
      case OrderFeedbackKind::kFilled:
        stats_->completed = true;
        stats_->state = LiveOpenCloseSmokeState::kDone;
        return;
      case OrderFeedbackKind::kCancelled:
        if (event.cumulative_filled_quantity + kEpsilon >=
            stats_->close_quantity) {
          stats_->completed = true;
          stats_->state = LiveOpenCloseSmokeState::kDone;
          return;
        }
        SetError("smoke close order did not fill");
        return;
      case OrderFeedbackKind::kRejected:
        SetError("smoke close order rejected");
        return;
      case OrderFeedbackKind::kAccepted:
      case OrderFeedbackKind::kPartialFilled:
      case OrderFeedbackKind::kContinuityLost:
        return;
    }
  }

  [[nodiscard]] double ComputeOpenQuantity(double price) noexcept {
    assert(pair_ != nullptr);
    assert(detail::SmokeOrderMetadataValid(
        *pair_, detail::SmokeOrderMetadataUse::kNotionalSizedOrder));
    if (!std::isfinite(price) || price <= 0.0) {
      SetError("invalid smoke market price");
      return 0.0;
    }
    const strategy::leadlag::InstrumentMetadata& instrument =
        pair_->lag_instrument;

    stats_->target_notional = pair_->execute.open_notional;
    const double raw_quantity =
        pair_->execute.open_notional / (price * instrument.notional_multiplier);
    double quantity = FloorToStep(raw_quantity, instrument.quantity_step);
    if (instrument.min_quantity > 0.0 && quantity < instrument.min_quantity) {
      quantity = instrument.min_quantity;
      stats_->used_min_quantity = true;
    }
    if (instrument.max_quantity > 0.0 && quantity > instrument.max_quantity) {
      quantity = FloorToStep(instrument.max_quantity, instrument.quantity_step);
    }

    const double estimated_notional =
        quantity * price * instrument.notional_multiplier;
    stats_->estimated_open_notional = estimated_notional;
    if (stats_->used_min_quantity &&
        estimated_notional > options_.max_notional + kEpsilon) {
      SetError("minimum notional exceeds cap");
      return 0.0;
    }
    if (!std::isfinite(quantity) || quantity <= kEpsilon) {
      SetError("invalid smoke quantity");
      return 0.0;
    }
    return quantity;
  }

  [[nodiscard]] double RoundedPrice(double price,
                                    OrderSide side) const noexcept {
    assert(pair_ != nullptr);
    const strategy::leadlag::InstrumentMetadata& instrument =
        pair_->lag_instrument;
    assert(std::isfinite(instrument.price_tick));
    assert(instrument.price_tick > 0.0);
    if (!std::isfinite(price) || price <= 0.0) {
      return 0.0;
    }
    const double scaled = price / instrument.price_tick;
    const double units = side == OrderSide::kBuy
                             ? std::ceil(scaled - kEpsilon)
                             : std::floor(scaled + kEpsilon);
    const double rounded = units * instrument.price_tick;
    return std::isfinite(rounded) && rounded > 0.0 ? rounded : 0.0;
  }

  [[nodiscard]] static double FloorToStep(double quantity,
                                          double step) noexcept {
    assert(std::isfinite(step));
    assert(step > 0.0);
    if (!std::isfinite(quantity)) {
      return 0.0;
    }
    return std::floor(quantity / step + kEpsilon) * step;
  }

  [[nodiscard]] static std::string FormatPrice(double price,
                                               std::int32_t decimal_places) {
    assert(decimal_places >= 0);
    assert(decimal_places < detail::kSmokeOrderDecimalPlaceLimit);
    return fmt::format("{:.{}f}", price, decimal_places);
  }

  [[nodiscard]] std::string_view GateSymbol() const noexcept {
    if (!pair_->lag_instrument.exchange_symbol.empty()) {
      return pair_->lag_instrument.exchange_symbol;
    }
    return pair_->symbol;
  }

  void SetError(std::string error) noexcept {
    if (stats_->error.empty()) {
      stats_->error = std::move(error);
    }
    stats_->state = LiveOpenCloseSmokeState::kError;
  }

  strategy::leadlag::Config config_;
  LiveOpenCloseSmokeOptions options_;
  LiveOpenCloseSmokeStats owned_stats_;
  LiveOpenCloseSmokeStats* stats_{&owned_stats_};
  const strategy::leadlag::PairConfig* pair_{nullptr};
  std::string open_price_text_;
  std::string open_quantity_text_;
  std::string close_price_text_;
  std::string close_quantity_text_;
  double pending_close_quantity_{0.0};
};

class LiveUnfilledCancelSmokeStrategy {
 public:
  LiveUnfilledCancelSmokeStrategy(strategy::leadlag::Config config,
                                  LiveUnfilledCancelSmokeOptions options,
                                  LiveUnfilledCancelSmokeStats* stats)
      : config_(std::move(config)),
        options_(std::move(options)),
        stats_(stats == nullptr ? &owned_stats_ : stats) {
    pair_ = FindSmokePair();
    if (pair_ == nullptr) {
      SetError("smoke symbol not found in lead_lag config");
      return;
    }
    ValidateOptions();
    if (!detail::SmokeOrderMetadataValid(
            *pair_, detail::SmokeOrderMetadataUse::kNotionalSizedOrder)) {
      SetError("invalid smoke instrument sizing metadata");
    }
  }

  template <typename ContextT>
  void OnBookTicker(const BookTicker& ticker, ContextT& context) noexcept {
    ++stats_->book_tickers;
    if (pair_ == nullptr) {
      return;
    }
    if (ticker.exchange != Exchange::kGate ||
        ticker.symbol_id != pair_->symbol_id) {
      return;
    }
    if (stats_->state == LiveUnfilledCancelSmokeState::kWaitingTicker) {
      SubmitPassiveOpen(ticker, context);
    }
  }

  template <typename ContextT>
  void OnOrderResponse(const core::OrderResponseEvent& event,
                       ContextT& context) noexcept {
    ++stats_->order_responses;
    if (stats_->open_local_order_id == 0 ||
        event.local_order_id != stats_->open_local_order_id) {
      return;
    }
    switch (event.kind) {
      case core::OrderResponseKind::kAccepted:
        RequestCancel(context);
        return;
      case core::OrderResponseKind::kRejected:
        SetError("smoke unfilled order rejected by order session");
        return;
      case core::OrderResponseKind::kCancelRejected:
        SetError("smoke unfilled cancel rejected by order session");
        return;
      case core::OrderResponseKind::kAck:
      case core::OrderResponseKind::kCancelAccepted:
        return;
    }
  }

  template <typename ContextT>
  void OnOrderFeedback(const OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    ++stats_->order_feedbacks;
    if (event.kind == OrderFeedbackKind::kContinuityLost) {
      stats_->continuity_lost_stop_requested = true;
      SetError("feedback continuity lost");
      return;
    }
    if (stats_->open_local_order_id == 0 ||
        event.local_order_id != stats_->open_local_order_id) {
      return;
    }
    switch (event.kind) {
      case OrderFeedbackKind::kAccepted:
        RequestCancel(context);
        return;
      case OrderFeedbackKind::kCancelled:
        CompleteCancelled(event);
        return;
      case OrderFeedbackKind::kRejected:
        SetError("smoke unfilled order rejected");
        return;
      case OrderFeedbackKind::kPartialFilled:
      case OrderFeedbackKind::kFilled:
        SetError("unexpected fill before cancel");
        return;
      case OrderFeedbackKind::kContinuityLost:
        return;
    }
  }

  [[nodiscard]] bool ShouldStop() const noexcept {
    return stats_->state == LiveUnfilledCancelSmokeState::kDone ||
           stats_->state == LiveUnfilledCancelSmokeState::kError ||
           stats_->continuity_lost_stop_requested;
  }

  [[nodiscard]] const LiveUnfilledCancelSmokeStats& stats() const noexcept {
    return *stats_;
  }

 private:
  static constexpr double kEpsilon = 1e-9;

  const strategy::leadlag::PairConfig* FindSmokePair() const noexcept {
    if (options_.symbol.empty()) {
      return config_.pairs.empty() ? nullptr : &config_.pairs.front();
    }
    for (const strategy::leadlag::PairConfig& pair : config_.pairs) {
      if (pair.symbol == options_.symbol ||
          pair.lag_instrument.exchange_symbol == options_.symbol) {
        return &pair;
      }
    }
    return nullptr;
  }

  void ValidateOptions() noexcept {
    if (!std::isfinite(options_.passive_price_bps) ||
        options_.passive_price_bps < 0.0 ||
        options_.passive_price_bps >= 10000.0) {
      SetError("smoke passive price bps must be in [0, 10000)");
      return;
    }
    if (!std::isfinite(options_.max_notional) || options_.max_notional <= 0.0) {
      SetError("smoke max notional must be positive");
    }
  }

  template <typename ContextT>
  void SubmitPassiveOpen(const BookTicker& ticker, ContextT& context) noexcept {
    const double order_price = RoundedDownPrice(
        ticker.bid_price * (1.0 - options_.passive_price_bps / 10000.0));
    if (order_price <= 0.0) {
      SetError("invalid smoke passive price");
      return;
    }
    const double quantity = ComputeOpenQuantity(order_price);
    if (quantity <= kEpsilon) {
      return;
    }

    open_price_text_ =
        FormatPrice(order_price, pair_->lag_instrument.price_decimal_places);
    open_quantity_text_ =
        FormatPrice(quantity, pair_->lag_instrument.quantity_decimal_places);
    const core::OrderPlaceResult placed =
        context.PlaceOrder(core::OrderCreateRequest{
            .exchange = Exchange::kGate,
            .symbol_id = pair_->symbol_id,
            .symbol = GateSymbol(),
            .side = OrderSide::kBuy,
            .order_type = OrderType::kLimit,
            .time_in_force = TimeInForce::kGoodTillCancel,
            .quantity = quantity,
            .quantity_text = open_quantity_text_,
            .price_text = open_price_text_,
            .reduce_only = false,
        });
    if (placed.status != core::OrderPlaceStatus::kOk ||
        placed.local_order_id == 0) {
      SetError("failed to place smoke unfilled order");
      return;
    }

    stats_->open_local_order_id = placed.local_order_id;
    stats_->open_quantity = quantity;
    stats_->state = LiveUnfilledCancelSmokeState::kOpenPending;
  }

  template <typename ContextT>
  void RequestCancel(ContextT& context) noexcept {
    if (stats_->completed ||
        stats_->state == LiveUnfilledCancelSmokeState::kDone ||
        stats_->state == LiveUnfilledCancelSmokeState::kError) {
      return;
    }
    if (stats_->cancel_requested) {
      return;
    }
    if (stats_->open_local_order_id == 0) {
      SetError("invalid smoke unfilled local order id");
      return;
    }
    const core::OrderCancelResult cancelled =
        context.CancelOrder(stats_->open_local_order_id);
    if (cancelled.status != core::OrderCancelStatus::kOk) {
      SetError("failed to cancel smoke unfilled order");
      return;
    }
    stats_->cancel_requested = true;
    stats_->state = LiveUnfilledCancelSmokeState::kCancelPending;
  }

  void CompleteCancelled(const OrderFeedbackEvent& event) noexcept {
    if (std::abs(event.cumulative_filled_quantity) > kEpsilon) {
      SetError("unexpected fill before cancel");
      return;
    }
    if (std::abs(event.cancelled_quantity - stats_->open_quantity) > kEpsilon) {
      SetError("unexpected cancelled quantity");
      return;
    }
    stats_->completed = true;
    stats_->state = LiveUnfilledCancelSmokeState::kDone;
  }

  [[nodiscard]] double ComputeOpenQuantity(double price) noexcept {
    assert(pair_ != nullptr);
    assert(detail::SmokeOrderMetadataValid(
        *pair_, detail::SmokeOrderMetadataUse::kNotionalSizedOrder));
    if (!std::isfinite(price) || price <= 0.0) {
      SetError("invalid smoke market price");
      return 0.0;
    }
    const strategy::leadlag::InstrumentMetadata& instrument =
        pair_->lag_instrument;

    stats_->target_notional = pair_->execute.open_notional;
    const double raw_quantity =
        pair_->execute.open_notional / (price * instrument.notional_multiplier);
    double quantity = FloorToStep(raw_quantity, instrument.quantity_step);
    if (instrument.min_quantity > 0.0 && quantity < instrument.min_quantity) {
      quantity = instrument.min_quantity;
      stats_->used_min_quantity = true;
    }
    if (instrument.max_quantity > 0.0 && quantity > instrument.max_quantity) {
      quantity = FloorToStep(instrument.max_quantity, instrument.quantity_step);
    }

    const double estimated_notional =
        quantity * price * instrument.notional_multiplier;
    stats_->estimated_open_notional = estimated_notional;
    if (stats_->used_min_quantity &&
        estimated_notional > options_.max_notional + kEpsilon) {
      SetError("minimum notional exceeds cap");
      return 0.0;
    }
    if (!std::isfinite(quantity) || quantity <= kEpsilon) {
      SetError("invalid smoke quantity");
      return 0.0;
    }
    return quantity;
  }

  [[nodiscard]] double RoundedDownPrice(double price) const noexcept {
    assert(pair_ != nullptr);
    const strategy::leadlag::InstrumentMetadata& instrument =
        pair_->lag_instrument;
    assert(std::isfinite(instrument.price_tick));
    assert(instrument.price_tick > 0.0);
    if (!std::isfinite(price) || price <= 0.0) {
      return 0.0;
    }
    const double scaled = price / instrument.price_tick;
    const double rounded =
        std::floor(scaled + kEpsilon) * instrument.price_tick;
    return std::isfinite(rounded) && rounded > 0.0 ? rounded : 0.0;
  }

  [[nodiscard]] static double FloorToStep(double quantity,
                                          double step) noexcept {
    assert(std::isfinite(step));
    assert(step > 0.0);
    if (!std::isfinite(quantity)) {
      return 0.0;
    }
    return std::floor(quantity / step + kEpsilon) * step;
  }

  [[nodiscard]] static std::string FormatPrice(double price,
                                               std::int32_t decimal_places) {
    assert(decimal_places >= 0);
    assert(decimal_places < detail::kSmokeOrderDecimalPlaceLimit);
    return fmt::format("{:.{}f}", price, decimal_places);
  }

  [[nodiscard]] std::string_view GateSymbol() const noexcept {
    if (!pair_->lag_instrument.exchange_symbol.empty()) {
      return pair_->lag_instrument.exchange_symbol;
    }
    return pair_->symbol;
  }

  void SetError(std::string error) noexcept {
    if (stats_->error.empty()) {
      stats_->error = std::move(error);
    }
    stats_->state = LiveUnfilledCancelSmokeState::kError;
  }

  strategy::leadlag::Config config_;
  LiveUnfilledCancelSmokeOptions options_;
  LiveUnfilledCancelSmokeStats owned_stats_;
  LiveUnfilledCancelSmokeStats* stats_{&owned_stats_};
  const strategy::leadlag::PairConfig* pair_{nullptr};
  std::string open_price_text_;
  std::string open_quantity_text_;
};

class LiveSubmitRejectSmokeStrategy {
 public:
  LiveSubmitRejectSmokeStrategy(strategy::leadlag::Config config,
                                LiveSubmitRejectSmokeOptions options,
                                LiveSubmitRejectSmokeStats* stats)
      : config_(std::move(config)),
        options_(std::move(options)),
        stats_(stats == nullptr ? &owned_stats_ : stats) {
    pair_ = FindSmokePair();
    if (pair_ == nullptr) {
      SetError("smoke symbol not found in lead_lag config");
      return;
    }
    ValidateOptions();
    if (!detail::SmokeOrderMetadataValid(
            *pair_, detail::SmokeOrderMetadataUse::kMinimumQuantityOrder)) {
      SetError("invalid submit reject smoke instrument sizing metadata");
    }
  }

  template <typename ContextT>
  void OnBookTicker(const BookTicker& ticker, ContextT& context) noexcept {
    ++stats_->book_tickers;
    if (pair_ == nullptr) {
      return;
    }
    if (ticker.exchange != Exchange::kGate ||
        ticker.symbol_id != pair_->symbol_id) {
      return;
    }
    if (stats_->state == LiveSubmitRejectSmokeState::kWaitingTicker) {
      SubmitOrder(ticker, context);
    }
  }

  template <typename ContextT>
  void OnOrderResponse(const core::OrderResponseEvent& event,
                       ContextT& context) noexcept {
    ++stats_->order_responses;
    if (stats_->local_order_id == 0 ||
        event.local_order_id != stats_->local_order_id) {
      return;
    }
    switch (event.kind) {
      case core::OrderResponseKind::kAck:
        return;
      case core::OrderResponseKind::kRejected:
        CompleteRejected();
        return;
      case core::OrderResponseKind::kAccepted:
        SetError("submit reject smoke order accepted unexpectedly");
        return;
      case core::OrderResponseKind::kCancelRejected:
        SetError("submit reject smoke cancel rejected unexpectedly");
        return;
      case core::OrderResponseKind::kCancelAccepted:
        SetError("submit reject smoke cancel accepted unexpectedly");
        return;
    }
  }

  template <typename ContextT>
  void OnOrderFeedback(const OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    ++stats_->order_feedbacks;
    if (event.kind == OrderFeedbackKind::kContinuityLost) {
      stats_->continuity_lost_stop_requested = true;
      SetError("feedback continuity lost");
      return;
    }
    if (stats_->local_order_id == 0 ||
        event.local_order_id != stats_->local_order_id) {
      return;
    }
    switch (event.kind) {
      case OrderFeedbackKind::kAccepted:
      case OrderFeedbackKind::kCancelled:
      case OrderFeedbackKind::kRejected:
      case OrderFeedbackKind::kPartialFilled:
      case OrderFeedbackKind::kFilled:
        SetError("unexpected private feedback for submit reject smoke");
        return;
      case OrderFeedbackKind::kContinuityLost:
        return;
    }
  }

  [[nodiscard]] bool ShouldStop() const noexcept {
    return stats_->state == LiveSubmitRejectSmokeState::kDone ||
           stats_->state == LiveSubmitRejectSmokeState::kError ||
           stats_->continuity_lost_stop_requested;
  }

  [[nodiscard]] const LiveSubmitRejectSmokeStats& stats() const noexcept {
    return *stats_;
  }

 private:
  static constexpr double kEpsilon = 1e-9;

  const strategy::leadlag::PairConfig* FindSmokePair() const noexcept {
    if (options_.symbol.empty()) {
      return config_.pairs.empty() ? nullptr : &config_.pairs.front();
    }
    for (const strategy::leadlag::PairConfig& pair : config_.pairs) {
      if (pair.symbol == options_.symbol ||
          pair.lag_instrument.exchange_symbol == options_.symbol) {
        return &pair;
      }
    }
    return nullptr;
  }

  void ValidateOptions() noexcept {
    if (!std::isfinite(options_.max_notional) || options_.max_notional <= 0.0) {
      SetError("smoke max notional must be positive");
    }
  }

  template <typename ContextT>
  void SubmitOrder(const BookTicker& ticker, ContextT& context) noexcept {
    if (!std::isfinite(ticker.bid_price) || ticker.bid_price <= 0.0) {
      SetError("invalid submit reject smoke market price");
      return;
    }
    const double order_price = pair_->lag_instrument.price_tick;
    assert(order_price > 0.0);
    const double quantity = ComputeQuantity();
    if (quantity <= kEpsilon) {
      return;
    }

    price_text_ =
        FormatPrice(order_price, pair_->lag_instrument.price_decimal_places);
    quantity_text_ =
        FormatPrice(quantity, pair_->lag_instrument.quantity_decimal_places);
    const core::OrderPlaceResult placed =
        context.PlaceOrder(core::OrderCreateRequest{
            .exchange = Exchange::kGate,
            .symbol_id = pair_->symbol_id,
            .symbol = GateSymbol(),
            .side = OrderSide::kBuy,
            .order_type = OrderType::kLimit,
            .time_in_force = TimeInForce::kImmediateOrCancel,
            .quantity = quantity,
            .quantity_text = quantity_text_,
            .price_text = price_text_,
            .reduce_only = true,
        });
    if (placed.status != core::OrderPlaceStatus::kOk ||
        placed.local_order_id == 0) {
      SetError("failed to place submit reject smoke order");
      return;
    }

    stats_->local_order_id = placed.local_order_id;
    stats_->quantity = quantity;
    stats_->state = LiveSubmitRejectSmokeState::kOrderPending;
  }

  void CompleteRejected() noexcept {
    stats_->rejected_seen = true;
    stats_->completed = true;
    stats_->state = LiveSubmitRejectSmokeState::kDone;
  }

  [[nodiscard]] double ComputeQuantity() noexcept {
    assert(pair_ != nullptr);
    assert(detail::SmokeOrderMetadataValid(
        *pair_, detail::SmokeOrderMetadataUse::kMinimumQuantityOrder));
    const strategy::leadlag::InstrumentMetadata& instrument =
        pair_->lag_instrument;

    stats_->target_notional = pair_->execute.open_notional;
    double quantity =
        FloorToStep(instrument.min_quantity, instrument.quantity_step);
    stats_->used_min_quantity = true;
    if (instrument.max_quantity > 0.0 && quantity > instrument.max_quantity) {
      quantity = FloorToStep(instrument.max_quantity, instrument.quantity_step);
    }

    const double estimated_notional =
        quantity * instrument.price_tick * instrument.notional_multiplier;
    stats_->estimated_notional = estimated_notional;
    if (stats_->used_min_quantity &&
        estimated_notional > options_.max_notional + kEpsilon) {
      SetError("minimum notional exceeds cap");
      return 0.0;
    }
    if (!std::isfinite(quantity) || quantity <= kEpsilon) {
      SetError("invalid submit reject smoke quantity");
      return 0.0;
    }
    return quantity;
  }

  [[nodiscard]] static double FloorToStep(double quantity,
                                          double step) noexcept {
    assert(std::isfinite(step));
    assert(step > 0.0);
    if (!std::isfinite(quantity)) {
      return 0.0;
    }
    return std::floor(quantity / step + kEpsilon) * step;
  }

  [[nodiscard]] static std::string FormatPrice(double price,
                                               std::int32_t decimal_places) {
    assert(decimal_places >= 0);
    assert(decimal_places < detail::kSmokeOrderDecimalPlaceLimit);
    return fmt::format("{:.{}f}", price, decimal_places);
  }

  [[nodiscard]] std::string_view GateSymbol() const noexcept {
    if (!pair_->lag_instrument.exchange_symbol.empty()) {
      return pair_->lag_instrument.exchange_symbol;
    }
    return pair_->symbol;
  }

  void SetError(std::string error) noexcept {
    if (stats_->error.empty()) {
      stats_->error = std::move(error);
    }
    stats_->state = LiveSubmitRejectSmokeState::kError;
  }

  strategy::leadlag::Config config_;
  LiveSubmitRejectSmokeOptions options_;
  LiveSubmitRejectSmokeStats owned_stats_;
  LiveSubmitRejectSmokeStats* stats_{&owned_stats_};
  const strategy::leadlag::PairConfig* pair_{nullptr};
  std::string price_text_;
  std::string quantity_text_;
};

[[nodiscard]] inline int ResolveLiveOrdersExitCode(
    int runtime_exit_code, const LiveOrdersStrategyStats& stats) noexcept {
  if (stats.continuity_lost_stop_requested) {
    return kContinuityLostEmergencyHandoffExitCode;
  }
  return runtime_exit_code;
}

[[nodiscard]] inline int ResolveLiveOpenCloseSmokeExitCode(
    int runtime_exit_code, const LiveOpenCloseSmokeStats& stats) noexcept {
  if (stats.continuity_lost_stop_requested) {
    return kContinuityLostEmergencyHandoffExitCode;
  }
  if (runtime_exit_code != 0) {
    return runtime_exit_code;
  }
  return stats.completed ? 0 : 1;
}

[[nodiscard]] inline int ResolveLiveUnfilledCancelSmokeExitCode(
    int runtime_exit_code, const LiveUnfilledCancelSmokeStats& stats) noexcept {
  if (stats.continuity_lost_stop_requested) {
    return kContinuityLostEmergencyHandoffExitCode;
  }
  if (runtime_exit_code != 0) {
    return runtime_exit_code;
  }
  return stats.completed ? 0 : 1;
}

[[nodiscard]] inline int ResolveLiveSubmitRejectSmokeExitCode(
    int runtime_exit_code, const LiveSubmitRejectSmokeStats& stats) noexcept {
  if (stats.continuity_lost_stop_requested) {
    return kContinuityLostEmergencyHandoffExitCode;
  }
  if (runtime_exit_code != 0) {
    return runtime_exit_code;
  }
  return stats.completed ? 0 : 1;
}

[[nodiscard]] inline std::uint8_t CountSmokeModes(
    SmokeModeSelection selection) noexcept {
  return static_cast<std::uint8_t>(selection.open_close) +
         static_cast<std::uint8_t>(selection.unfilled_cancel) +
         static_cast<std::uint8_t>(selection.submit_reject);
}

[[nodiscard]] inline RunModeResult ResolveRunMode(
    config::StrategyMode strategy_mode, bool connect_data, bool execute,
    SmokeModeSelection smoke_modes) {
  if (CountSmokeModes(smoke_modes) > 1) {
    return {.ok = false,
            .mode = RunMode::kValidateOnly,
            .error = "only one smoke mode may be selected"};
  }
  if (smoke_modes.open_close && !execute) {
    return {.ok = false,
            .mode = RunMode::kValidateOnly,
            .error = "--smoke-open-close requires --execute"};
  }
  if (smoke_modes.unfilled_cancel && !execute) {
    return {.ok = false,
            .mode = RunMode::kValidateOnly,
            .error = "--smoke-unfilled-cancel requires --execute"};
  }
  if (smoke_modes.submit_reject && !execute) {
    return {.ok = false,
            .mode = RunMode::kValidateOnly,
            .error = "--smoke-submit-reject requires --execute"};
  }
  if (execute && strategy_mode != config::StrategyMode::kLive) {
    return {.ok = false,
            .mode = RunMode::kValidateOnly,
            .error = "strategy.mode must be live when --execute is specified"};
  }
  if (smoke_modes.open_close) {
    return {.ok = true, .mode = RunMode::kLiveOpenCloseSmoke};
  }
  if (smoke_modes.unfilled_cancel) {
    return {.ok = true, .mode = RunMode::kLiveUnfilledCancelSmoke};
  }
  if (smoke_modes.submit_reject) {
    return {.ok = true, .mode = RunMode::kLiveSubmitRejectSmoke};
  }
  if (execute) {
    return {.ok = true, .mode = RunMode::kLiveOrders};
  }
  if (connect_data) {
    return {.ok = true, .mode = RunMode::kSignalOnly};
  }
  return {.ok = true, .mode = RunMode::kValidateOnly};
}

[[nodiscard]] inline RunModeResult ResolveRunMode(
    config::StrategyMode strategy_mode, bool connect_data, bool execute,
    bool smoke_open_close = false, bool smoke_unfilled_cancel = false) {
  return ResolveRunMode(strategy_mode, connect_data, execute,
                        SmokeModeSelection{
                            .open_close = smoke_open_close,
                            .unfilled_cancel = smoke_unfilled_cancel,
                        });
}

[[nodiscard]] inline const char* RunModeName(RunMode mode) noexcept {
  switch (mode) {
    case RunMode::kValidateOnly:
      return "validate_only";
    case RunMode::kSignalOnly:
      return "signal_only";
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
    case RunMode::kMarketCalc:
      return "market_calc";
#endif
    case RunMode::kLiveOrders:
      return "live_orders";
    case RunMode::kLiveOpenCloseSmoke:
      return "live_open_close_smoke";
    case RunMode::kLiveUnfilledCancelSmoke:
      return "live_unfilled_cancel_smoke";
    case RunMode::kLiveSubmitRejectSmoke:
      return "live_submit_reject_smoke";
  }
  return "unknown";
}

[[nodiscard]] inline const char* RecoveryStateName(
    strategy::leadlag::RecoveryState state) noexcept {
  switch (state) {
    case strategy::leadlag::RecoveryState::kNormal:
      return "normal";
    case strategy::leadlag::RecoveryState::kDegradedNeedsReconcile:
      return "degraded_needs_reconcile";
    case strategy::leadlag::RecoveryState::kReconciling:
      return "reconciling";
    case strategy::leadlag::RecoveryState::kRecovered:
      return "recovered";
    case strategy::leadlag::RecoveryState::kManualIntervention:
      return "manual_intervention";
  }
  return "unknown";
}

[[nodiscard]] inline const char* LiveOpenCloseSmokeStateName(
    LiveOpenCloseSmokeState state) noexcept {
  switch (state) {
    case LiveOpenCloseSmokeState::kWaitingTicker:
      return "waiting_ticker";
    case LiveOpenCloseSmokeState::kOpenPending:
      return "open_pending";
    case LiveOpenCloseSmokeState::kWaitingCloseTicker:
      return "waiting_close_ticker";
    case LiveOpenCloseSmokeState::kClosePending:
      return "close_pending";
    case LiveOpenCloseSmokeState::kDone:
      return "done";
    case LiveOpenCloseSmokeState::kError:
      return "error";
  }
  return "unknown";
}

[[nodiscard]] inline const char* LiveUnfilledCancelSmokeStateName(
    LiveUnfilledCancelSmokeState state) noexcept {
  switch (state) {
    case LiveUnfilledCancelSmokeState::kWaitingTicker:
      return "waiting_ticker";
    case LiveUnfilledCancelSmokeState::kOpenPending:
      return "open_pending";
    case LiveUnfilledCancelSmokeState::kCancelPending:
      return "cancel_pending";
    case LiveUnfilledCancelSmokeState::kDone:
      return "done";
    case LiveUnfilledCancelSmokeState::kError:
      return "error";
  }
  return "unknown";
}

[[nodiscard]] inline const char* LiveSubmitRejectSmokeStateName(
    LiveSubmitRejectSmokeState state) noexcept {
  switch (state) {
    case LiveSubmitRejectSmokeState::kWaitingTicker:
      return "waiting_ticker";
    case LiveSubmitRejectSmokeState::kOrderPending:
      return "order_pending";
    case LiveSubmitRejectSmokeState::kDone:
      return "done";
    case LiveSubmitRejectSmokeState::kError:
      return "error";
  }
  return "unknown";
}

[[nodiscard]] inline const char* BoolName(bool value) noexcept {
  return value ? "true" : "false";
}

[[nodiscard]] inline std::string FormatRecoveryDiagnosticsFields(
    const RecoveryDiagnostics& diagnostics) {
  std::string fields;
  fields.reserve(128);
  fields.append("recovery_state=");
  fields.append(RecoveryStateName(diagnostics.recovery_state));
  fields.append(" needs_reconcile=");
  fields.append(BoolName(diagnostics.needs_reconcile));
  fields.append(" manual_intervention=");
  fields.append(BoolName(diagnostics.manual_intervention));
  fields.append(" new_entries_paused=");
  fields.append(BoolName(diagnostics.new_entries_paused));
  return fields;
}

}  // namespace aquila::tools::lead_lag

#endif  // AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_
