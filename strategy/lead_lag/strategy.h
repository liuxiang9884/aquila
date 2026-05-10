#ifndef AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
#define AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_

#include <utility>

#include "core/market_data/types.h"
#include "core/strategy/order_types.h"
#include "core/trading/order_feedback_event.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/raw_market_state.h"

namespace aquila::strategy::leadlag {

class Strategy {
 public:
  explicit Strategy(Config config) : config_(std::move(config)) {
    raw_market_state_.Reset(config_);
  }

  Strategy(const Strategy&) = delete;
  Strategy& operator=(const Strategy&) = delete;
  Strategy(Strategy&&) noexcept = default;
  Strategy& operator=(Strategy&&) noexcept = default;

  template <typename ContextT>
  void OnStart(ContextT&) noexcept {}

  template <typename ContextT>
  void OnBookTicker(const BookTicker& ticker, ContextT&) noexcept {
    last_market_update_ = raw_market_state_.OnBookTicker(ticker);
  }

  template <typename ContextT>
  void OnOrderResponse(const strategy::OrderResponseEvent&,
                       ContextT&) noexcept {}

  template <typename ContextT>
  void OnOrderFeedback(const OrderFeedbackEvent& event, ContextT&) noexcept {
    if (event.kind == OrderFeedbackKind::kGap) {
      degraded_ = true;
    }
  }

  template <typename ContextT>
  void OnLoop(ContextT&) noexcept {}

  template <typename ContextT>
  void OnIdle(ContextT&) noexcept {}

  template <typename ContextT>
  void OnStop(ContextT&) noexcept {}

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

 private:
  Config config_;
  RawMarketState raw_market_state_;
  MarketUpdate last_market_update_;
  bool degraded_{false};
  bool stop_requested_{false};
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_STRATEGY_H_
