#ifndef AQUILA_CORE_STRATEGY_STRATEGY_RUNTIME_H_
#define AQUILA_CORE_STRATEGY_STRATEGY_RUNTIME_H_

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <utility>

#include "core/common/result.h"
#include "core/config/strategy_config.h"
#include "core/market_data/data_reader.h"
#include "core/market_data/types.h"
#include "core/strategy/order_manager.h"
#include "core/strategy/strategy_context.h"
#include "core/trading/order_feedback_shm.h"

namespace aquila::strategy {

template <typename UserStrategyT, typename OrderSessionT>
class StrategyRuntime {
 public:
  using OrderManagerT = OrderManager<OrderSessionT>;
  using ContextT = StrategyContext<OrderSessionT>;

  StrategyRuntime(const StrategyRuntime&) = delete;
  StrategyRuntime& operator=(const StrategyRuntime&) = delete;
  StrategyRuntime(StrategyRuntime&&) = delete;
  StrategyRuntime& operator=(StrategyRuntime&&) = delete;

  template <typename OrderSessionFactoryT, typename... UserStrategyArgs>
  static Result<std::unique_ptr<StrategyRuntime>> CreateForTest(
      config::StrategyConfig config,
      OrderSessionFactoryT&& order_session_factory,
      UserStrategyArgs&&... user_strategy_args) noexcept {
    Result<std::unique_ptr<StrategyRuntime>> result;
    if (config.order_capacity == 0) {
      result.error = "strategy.order_capacity must be positive";
      return result;
    }

    const std::size_t order_capacity = config.order_capacity;
    const std::uint8_t strategy_id =
        static_cast<std::uint8_t>(config.strategy_id);

    std::unique_ptr<StrategyRuntime> runtime(new (std::nothrow)
                                                 StrategyRuntime());
    if (runtime == nullptr) {
      result.error = "strategy runtime allocation failed";
      return result;
    }

    try {
      runtime->config_ = std::move(config);
      runtime->order_session_.emplace(
          std::forward<OrderSessionFactoryT>(order_session_factory)());
      runtime->order_manager_.emplace(*runtime->order_session_, order_capacity,
                                      strategy_id);
      runtime->context_.emplace(*runtime->order_manager_);
      runtime->user_strategy_.emplace(
          std::forward<UserStrategyArgs>(user_strategy_args)...);
    } catch (const std::exception& exception) {
      result.error = exception.what();
      return result;
    } catch (...) {
      result.error = "strategy runtime create failed";
      return result;
    }

    result.value = std::move(runtime);
    result.ok = true;
    return result;
  }

  int Run() noexcept {
    return 0;
  }

  void HandleBookTickerForTest(const BookTicker& ticker) noexcept {
    OnBookTicker(ticker);
  }

  void HandleOrderFeedbackForTest(const OrderFeedbackEvent& event) noexcept {
    OnOrderFeedback(event);
  }

 private:
  StrategyRuntime() noexcept = default;

  void OnBookTicker(const BookTicker& ticker) noexcept {
    user_strategy_->OnBookTicker(ticker, *context_);
  }

  void OnOrderFeedback(const OrderFeedbackEvent& event) noexcept {
    order_manager_->OnOrderFeedback(event);
    user_strategy_->OnOrderFeedback(event, *context_);
  }

  config::StrategyConfig config_;
  std::optional<market_data::DataReader<>> data_reader_;
  std::optional<OrderSessionT> order_session_;
  std::optional<OrderManagerT> order_manager_;
  std::optional<ContextT> context_;
  std::optional<OrderFeedbackShmReader> feedback_reader_;
  std::optional<UserStrategyT> user_strategy_;
};

}  // namespace aquila::strategy

#endif  // AQUILA_CORE_STRATEGY_STRATEGY_RUNTIME_H_
