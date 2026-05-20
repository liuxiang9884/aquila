#ifndef AQUILA_TOOLS_GATE_DEMO_STRATEGY_H_
#define AQUILA_TOOLS_GATE_DEMO_STRATEGY_H_

#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/common/types.h"
#include "core/market_data/types.h"
#include "core/strategy/order_types.h"
#include "core/trading/order_feedback_event.h"

namespace aquila::tools::gate_demo_strategy {

struct DemoStrategyConfig {
  std::string contract{"BTC_USDT"};
  std::int32_t symbol_id{0};
  std::uint32_t wait_seconds{1};
  std::uint32_t rounds{1};
};

enum class DemoStrategyState : std::uint8_t {
  kWaitingTicker,
  kBuyPending,
  kCancelPending,
  kClosePending,
  kDone,
  kError,
};

using DemoStrategyConfigResult = Result<DemoStrategyConfig>;

namespace detail {

[[nodiscard]] inline DemoStrategyConfigResult DemoConfigFailure(
    std::string error) {
  DemoStrategyConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] inline DemoStrategyConfigResult DemoConfigSuccess(
    DemoStrategyConfig config) {
  DemoStrategyConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

[[nodiscard]] inline bool ReadRequiredString(const toml::table& table,
                                             std::string_view key,
                                             std::string* output,
                                             std::string* error) {
  const std::optional<std::string> value = table[key].value<std::string>();
  if (!value || value->empty()) {
    *error = fmt::format("demo.{} is required", key);
    return false;
  }
  *output = *value;
  return true;
}

[[nodiscard]] inline bool ReadRequiredInt32(const toml::table& table,
                                            std::string_view key,
                                            std::int32_t* output,
                                            std::string* error) {
  const std::optional<std::int64_t> value = table[key].value<std::int64_t>();
  if (!value) {
    *error = fmt::format("demo.{} is required", key);
    return false;
  }
  if (*value < std::numeric_limits<std::int32_t>::min() ||
      *value > std::numeric_limits<std::int32_t>::max()) {
    *error = fmt::format("demo.{} exceeds int32 range", key);
    return false;
  }
  *output = static_cast<std::int32_t>(*value);
  return true;
}

[[nodiscard]] inline bool ReadRequiredUInt32(const toml::table& table,
                                             std::string_view key,
                                             bool allow_zero,
                                             std::uint32_t* output,
                                             std::string* error) {
  const std::optional<std::int64_t> value = table[key].value<std::int64_t>();
  if (!value) {
    *error = fmt::format("demo.{} is required", key);
    return false;
  }
  if (*value < 0 || (!allow_zero && *value == 0)) {
    *error = fmt::format("demo.{} must be {}", key,
                         allow_zero ? "non-negative" : "positive");
    return false;
  }
  if (static_cast<std::uint64_t>(*value) >
      std::numeric_limits<std::uint32_t>::max()) {
    *error = fmt::format("demo.{} exceeds uint32 range", key);
    return false;
  }
  *output = static_cast<std::uint32_t>(*value);
  return true;
}

}  // namespace detail

[[nodiscard]] inline DemoStrategyConfigResult ParseDemoStrategyConfig(
    const toml::table& root) {
  const toml::table* demo = root["demo"].as_table();
  if (demo == nullptr) {
    return detail::DemoConfigFailure("demo section is required");
  }

  DemoStrategyConfig config;
  std::string error;
  if (!detail::ReadRequiredString(*demo, "contract", &config.contract,
                                  &error)) {
    return detail::DemoConfigFailure(std::move(error));
  }
  if (!detail::ReadRequiredInt32(*demo, "symbol_id", &config.symbol_id,
                                 &error)) {
    return detail::DemoConfigFailure(std::move(error));
  }
  if (!detail::ReadRequiredUInt32(*demo, "wait_seconds", true,
                                  &config.wait_seconds, &error)) {
    return detail::DemoConfigFailure(std::move(error));
  }
  if (!detail::ReadRequiredUInt32(*demo, "rounds", false, &config.rounds,
                                  &error)) {
    return detail::DemoConfigFailure(std::move(error));
  }
  return detail::DemoConfigSuccess(std::move(config));
}

class DemoStrategy {
 public:
  explicit DemoStrategy(DemoStrategyConfig config)
      : config_(std::move(config)) {
    if (config_.rounds == 0) {
      state_ = DemoStrategyState::kDone;
    }
  }

  template <typename ContextT>
  void OnBookTicker(const BookTicker& ticker, ContextT& context) noexcept {
    if (state_ != DemoStrategyState::kWaitingTicker || ShouldStop()) {
      return;
    }
    if (ticker.exchange != Exchange::kGate ||
        ticker.symbol_id != config_.symbol_id) {
      return;
    }

    buy_price_texts_.push_back(fmt::format("{:.12g}", ticker.ask_price));
    const std::string_view price_text = buy_price_texts_.back();
    const strategy::OrderPlaceResult placed =
        context.PlaceLimitOrder(strategy::OrderCreateRequest{
            .exchange = Exchange::kGate,
            .symbol_id = config_.symbol_id,
            .symbol = config_.contract,
            .side = OrderSide::kBuy,
            .time_in_force = TimeInForce::kGoodTillCancel,
            .quantity = kQuantity,
            .price_text = price_text,
            .reduce_only = false,
        });
    if (placed.status != strategy::OrderPlaceStatus::kOk) {
      state_ = DemoStrategyState::kError;
      return;
    }

    buy_local_order_id_ = placed.local_order_id;
    close_local_order_id_ = 0;
    wait_deadline_ = Clock::now() + std::chrono::seconds(config_.wait_seconds);
    state_ = DemoStrategyState::kBuyPending;
  }

  template <typename ContextT>
  void OnOrderResponse(const strategy::OrderResponseEvent& event,
                       ContextT&) noexcept {
    if (event.local_order_id == buy_local_order_id_ &&
        event.kind == strategy::OrderResponseKind::kRejected) {
      state_ = DemoStrategyState::kError;
      return;
    }
    if (event.local_order_id == buy_local_order_id_ &&
        event.kind == strategy::OrderResponseKind::kCancelRejected) {
      state_ = DemoStrategyState::kError;
      return;
    }
    if (event.local_order_id == close_local_order_id_ &&
        event.kind == strategy::OrderResponseKind::kRejected) {
      state_ = DemoStrategyState::kError;
    }
  }

  template <typename ContextT>
  void OnOrderFeedback(const OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    if (event.kind == OrderFeedbackKind::kContinuityLost) {
      state_ = DemoStrategyState::kError;
      return;
    }
    if (buy_local_order_id_ != 0 &&
        event.local_order_id == buy_local_order_id_) {
      OnBuyFeedback(event, context);
      return;
    }
    if (close_local_order_id_ != 0 &&
        event.local_order_id == close_local_order_id_) {
      OnCloseFeedback(event);
    }
  }

  template <typename ContextT>
  void OnIdle(ContextT& context) noexcept {
    CheckDeadline(context);
  }

  template <typename ContextT>
  void OnLoop(ContextT& context) noexcept {
    CheckDeadline(context);
  }

  [[nodiscard]] bool ShouldStop() const noexcept {
    return state_ == DemoStrategyState::kDone ||
           state_ == DemoStrategyState::kError ||
           completed_rounds_ >= config_.rounds;
  }

  [[nodiscard]] DemoStrategyState state() const noexcept {
    return state_;
  }

  [[nodiscard]] std::uint32_t completed_rounds() const noexcept {
    return completed_rounds_;
  }

 private:
  using Clock = std::chrono::steady_clock;

  static constexpr std::int64_t kQuantity = 1;

  template <typename ContextT>
  void CheckDeadline(ContextT& context) noexcept {
    if (state_ != DemoStrategyState::kBuyPending) {
      return;
    }
    if (Clock::now() < wait_deadline_) {
      return;
    }

    const strategy::StrategyOrder* order =
        context.FindOrder(buy_local_order_id_);
    if (order == nullptr) {
      state_ = DemoStrategyState::kError;
      return;
    }
    if (order->status == strategy::OrderStatus::kFilled ||
        order->cumulative_filled_quantity >= kQuantity) {
      SubmitClose(context);
      return;
    }
    SubmitCancel(context);
  }

  template <typename ContextT>
  void OnBuyFeedback(const OrderFeedbackEvent& event,
                     ContextT& context) noexcept {
    switch (event.kind) {
      case OrderFeedbackKind::kFilled:
        if (state_ == DemoStrategyState::kCancelPending) {
          SubmitClose(context);
        }
        return;
      case OrderFeedbackKind::kCancelled:
        if (state_ == DemoStrategyState::kBuyPending ||
            state_ == DemoStrategyState::kCancelPending) {
          if (event.cumulative_filled_quantity > 0) {
            SubmitClose(context);
            return;
          }
          CompleteCycle();
        }
        return;
      case OrderFeedbackKind::kRejected:
        state_ = DemoStrategyState::kError;
        return;
      case OrderFeedbackKind::kAccepted:
      case OrderFeedbackKind::kPartialFilled:
      case OrderFeedbackKind::kContinuityLost:
        return;
    }
  }

  void OnCloseFeedback(const OrderFeedbackEvent& event) noexcept {
    switch (event.kind) {
      case OrderFeedbackKind::kFilled:
        if (state_ == DemoStrategyState::kClosePending) {
          CompleteCycle();
        }
        return;
      case OrderFeedbackKind::kRejected:
      case OrderFeedbackKind::kCancelled:
        state_ = DemoStrategyState::kError;
        return;
      case OrderFeedbackKind::kAccepted:
      case OrderFeedbackKind::kPartialFilled:
      case OrderFeedbackKind::kContinuityLost:
        return;
    }
  }

  template <typename ContextT>
  void SubmitCancel(ContextT& context) noexcept {
    const strategy::OrderCancelResult cancelled =
        context.CancelOrder(buy_local_order_id_);
    if (cancelled.status != strategy::OrderCancelStatus::kOk) {
      state_ = DemoStrategyState::kError;
      return;
    }
    state_ = DemoStrategyState::kCancelPending;
  }

  template <typename ContextT>
  void SubmitClose(ContextT& context) noexcept {
    const strategy::OrderPlaceResult placed =
        context.PlaceLimitOrder(strategy::OrderCreateRequest{
            .exchange = Exchange::kGate,
            .symbol_id = config_.symbol_id,
            .symbol = config_.contract,
            .side = OrderSide::kSell,
            .time_in_force = TimeInForce::kImmediateOrCancel,
            .quantity = kQuantity,
            .price_text = market_price_text_,
            .reduce_only = true,
        });
    if (placed.status != strategy::OrderPlaceStatus::kOk) {
      state_ = DemoStrategyState::kError;
      return;
    }
    close_local_order_id_ = placed.local_order_id;
    state_ = DemoStrategyState::kClosePending;
  }

  void CompleteCycle() noexcept {
    ++completed_rounds_;
    buy_local_order_id_ = 0;
    close_local_order_id_ = 0;
    state_ = completed_rounds_ >= config_.rounds
                 ? DemoStrategyState::kDone
                 : DemoStrategyState::kWaitingTicker;
  }

  DemoStrategyConfig config_;
  DemoStrategyState state_{DemoStrategyState::kWaitingTicker};
  std::uint32_t completed_rounds_{0};
  std::uint64_t buy_local_order_id_{0};
  std::uint64_t close_local_order_id_{0};
  Clock::time_point wait_deadline_{};
  // Keeps buy price buffers stable for OrderManager string_views.
  std::deque<std::string> buy_price_texts_;
  std::string market_price_text_{"0"};
};

}  // namespace aquila::tools::gate_demo_strategy

#endif  // AQUILA_TOOLS_GATE_DEMO_STRATEGY_H_
