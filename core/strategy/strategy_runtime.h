#ifndef AQUILA_CORE_STRATEGY_STRATEGY_RUNTIME_H_
#define AQUILA_CORE_STRATEGY_STRATEGY_RUNTIME_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include "core/common/result.h"
#include "core/config/data_reader_config.h"
#include "core/config/strategy_config.h"
#include "core/market_data/data_reader_concepts.h"
#include "core/market_data/realtime_data_reader.h"
#include "core/market_data/types.h"
#include "core/strategy/order_manager.h"
#include "core/strategy/strategy_context.h"
#include "core/trading/order_feedback_shm.h"
#include "core/websocket/runtime_policy.h"

namespace aquila::strategy {

template <typename UserStrategyT, typename OrderSessionT,
          typename DataReaderT = market_data::RealtimeDataReader<>>
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
      runtime->BindRuntimeIfSupported();
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

  template <typename OrderSessionFactoryT, typename... UserStrategyArgs>
  static Result<std::unique_ptr<StrategyRuntime>> Create(
      config::StrategyConfig config,
      config::DataReaderConfig data_reader_config,
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
      runtime->data_reader_poll_budget_ =
          data_reader_config.max_events_per_source;
      runtime->data_reader_.emplace(std::move(data_reader_config));
      runtime->order_session_.emplace(
          std::forward<OrderSessionFactoryT>(order_session_factory)());
      runtime->order_manager_.emplace(*runtime->order_session_, order_capacity,
                                      strategy_id);
      runtime->context_.emplace(*runtime->order_manager_);
      runtime->user_strategy_.emplace(
          std::forward<UserStrategyArgs>(user_strategy_args)...);
      runtime->BindRuntimeIfSupported();
    } catch (const std::exception& exception) {
      result.error = exception.what();
      return result;
    } catch (...) {
      result.error = "strategy runtime create failed";
      return result;
    }

    if (runtime->config_.feedback.enabled) {
      OrderFeedbackShmConfig shm_config{
          .shm_name = runtime->config_.feedback.shm_name,
          .channel_name = runtime->config_.feedback.channel_name,
          .create = false,
          .remove_existing = false,
      };
      auto manager_result = OrderFeedbackShmManager::Open(shm_config);
      if (!manager_result.ok) {
        result.error = std::move(manager_result.error);
        return result;
      }
      runtime->feedback_shm_manager_.emplace(std::move(manager_result.value));

      const std::uint64_t consumer_run_id = DefaultFeedbackConsumerRunId();
      auto reader_result = OrderFeedbackShmReader::Claim(
          runtime->feedback_shm_manager_->channel(), strategy_id,
          consumer_run_id, runtime->config_.feedback.force_claim);
      if (!reader_result.ok) {
        result.error = std::move(reader_result.error);
        return result;
      }
      runtime->feedback_reader_.emplace(std::move(reader_result.value));
    }

    result.value = std::move(runtime);
    result.ok = true;
    return result;
  }

  int Run() noexcept {
    if (!order_session_ || !context_ || !user_strategy_) {
      return 1;
    }
    if constexpr (requires(OrderSessionT& session) {
                    session.SetRuntimeHook(
                        static_cast<void*>(nullptr),
                        &StrategyRuntime::RuntimeHookCallback);
                  }) {
      return RunWithRuntimeHook();
    }
    if (!StartOrderSession()) {
      return 1;
    }

    ApplyLoopRuntimePolicy();
    CallOnStart();
    const auto loop_started_at = std::chrono::steady_clock::now();
    int exit_code = 0;
    for (;;) {
      if (ShouldStop() || MaxLoopSecondsElapsed(loop_started_at)) {
        break;
      }
      if (!OrderSessionRunning()) {
        exit_code = 1;
        break;
      }

      std::uint64_t handled = 0;
      handled += PollOrderResponses();
      handled += PollOrderFeedback();
      if (OrderSessionReady()) {
        handled += PollDataReader();
      }
      CallOnLoop();

      if (ShouldStop() || MaxLoopSecondsElapsed(loop_started_at)) {
        break;
      }

      if (handled == 0) {
        CallOnIdle();
        if (ShouldStop() || MaxLoopSecondsElapsed(loop_started_at)) {
          break;
        }
        if (config_.loop.idle_policy ==
            config::StrategyLoopIdlePolicy::kYield) {
          std::this_thread::yield();
        }
      }
    }

    CallOnStop();
    StopOrderSession();
    return exit_code;
  }

  void OnBookTicker(const BookTicker& ticker) noexcept {
    if constexpr (requires(UserStrategyT& strategy, const BookTicker& event,
                           ContextT& context) {
                    strategy.OnBookTicker(event, context);
                  }) {
      user_strategy_->OnBookTicker(ticker, *context_);
    }
  }

  void OnOrderResponse(const OrderResponseEvent& event) noexcept {
    order_manager_->OnOrderResponse(event);
    if constexpr (requires(UserStrategyT& strategy,
                           const OrderResponseEvent& response,
                           ContextT& context) {
                    strategy.OnOrderResponse(response, context);
                  }) {
      user_strategy_->OnOrderResponse(event, *context_);
    }
  }

  void OnOrderFeedback(const OrderFeedbackEvent& event) noexcept {
    order_manager_->OnOrderFeedback(event);
    if constexpr (requires(UserStrategyT& strategy,
                           const OrderFeedbackEvent& feedback,
                           ContextT& context) {
                    strategy.OnOrderFeedback(feedback, context);
                  }) {
      user_strategy_->OnOrderFeedback(event, *context_);
    }
  }

  void HandleBookTickerForTest(const BookTicker& ticker) noexcept {
    OnBookTicker(ticker);
  }

  void HandleOrderResponseForTest(const OrderResponseEvent& event) noexcept {
    OnOrderResponse(event);
  }

  void HandleOrderFeedbackForTest(const OrderFeedbackEvent& event) noexcept {
    OnOrderFeedback(event);
  }

 private:
  StrategyRuntime() noexcept = default;

  void BindRuntimeIfSupported() {
    if constexpr (requires(OrderSessionT& session, StrategyRuntime& runtime) {
                    session.BindRuntime(runtime);
                  }) {
      order_session_->BindRuntime(*this);
    }
  }

  static void RuntimeHookCallback(void* context) noexcept {
    static_cast<StrategyRuntime*>(context)->DriveHookOnce();
  }

  int RunWithRuntimeHook() noexcept {
    stop_order_session_requested_ = false;
    hook_user_started_ = false;
    hook_exit_code_ = 0;
    order_session_->SetRuntimeHook(static_cast<void*>(this),
                                   &StrategyRuntime::RuntimeHookCallback);

    ApplyLoopRuntimePolicy();
    hook_loop_started_at_ = std::chrono::steady_clock::now();
    if (!StartOrderSession()) {
      hook_exit_code_ = 1;
    }
    if (hook_user_started_) {
      CallOnStop();
    }
    RequestOrderSessionStop();
    return hook_exit_code_;
  }

  void DriveHookOnce() noexcept {
    if (!hook_user_started_) {
      hook_user_started_ = true;
      CallOnStart();
    }
    if (ShouldStop() || MaxLoopSecondsElapsed(hook_loop_started_at_)) {
      RequestOrderSessionStop();
      return;
    }
    if (!OrderSessionRunning()) {
      hook_exit_code_ = 1;
      RequestOrderSessionStop();
      return;
    }

    std::uint64_t handled = 0;
    handled += PollOrderResponses();
    handled += PollOrderFeedback();
    if (OrderSessionReady()) {
      handled += PollDataReader();
    }
    CallOnLoop();

    if (ShouldStop() || MaxLoopSecondsElapsed(hook_loop_started_at_)) {
      RequestOrderSessionStop();
      return;
    }
    if (handled == 0) {
      CallOnIdle();
      if (ShouldStop() || MaxLoopSecondsElapsed(hook_loop_started_at_)) {
        RequestOrderSessionStop();
      }
    }
  }

  void RequestOrderSessionStop() noexcept {
    if (stop_order_session_requested_) {
      return;
    }
    stop_order_session_requested_ = true;
    StopOrderSession();
  }

  [[nodiscard]] static std::uint64_t DefaultFeedbackConsumerRunId() noexcept {
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::uint64_t value = static_cast<std::uint64_t>(now);
    return value == 0 ? 1 : value;
  }

  [[nodiscard]] bool StartOrderSession() noexcept {
    if constexpr (requires(OrderSessionT& session) { session.Start(); }) {
      using StartResultT = decltype(std::declval<OrderSessionT&>().Start());
      if constexpr (std::is_void_v<StartResultT>) {
        order_session_->Start();
        return true;
      } else {
        return static_cast<bool>(order_session_->Start());
      }
    }
    return true;
  }

  void StopOrderSession() noexcept {
    if constexpr (requires(OrderSessionT& session) { session.Stop(); }) {
      order_session_->Stop();
    }
  }

  [[nodiscard]] bool OrderSessionReady() noexcept {
    if constexpr (requires(OrderSessionT& session) { session.Ready(); }) {
      return static_cast<bool>(order_session_->Ready());
    }
    return true;
  }

  [[nodiscard]] bool OrderSessionRunning() noexcept {
    if constexpr (requires(OrderSessionT& session) { session.Running(); }) {
      return static_cast<bool>(order_session_->Running());
    }
    return true;
  }

  void ApplyLoopRuntimePolicy() noexcept {
    if (config_.loop.bind_cpu_id < 0) {
      return;
    }
    websocket::RuntimePolicy runtime_policy;
    runtime_policy.affinity_mode = websocket::AffinityMode::kBestEffort;
    runtime_policy.io_cpu_id = config_.loop.bind_cpu_id;
    runtime_policy.lock_memory = false;
    runtime_policy.prefault_stack = false;
    (void)websocket::ApplyRuntimePolicy(runtime_policy);
  }

  [[nodiscard]] std::uint64_t PollOrderResponses() noexcept {
    if constexpr (requires(OrderSessionT& session, StrategyRuntime& runtime) {
                    session.PollOrderResponses(runtime);
                  }) {
      using PollResultT =
          decltype(std::declval<OrderSessionT&>().PollOrderResponses(
              std::declval<StrategyRuntime&>()));
      if constexpr (std::is_void_v<PollResultT>) {
        order_session_->PollOrderResponses(*this);
        return 0;
      } else {
        return static_cast<std::uint64_t>(
            order_session_->PollOrderResponses(*this));
      }
    }
    return 0;
  }

  [[nodiscard]] std::uint64_t PollOrderFeedback() noexcept {
    if (!feedback_reader_) {
      return 0;
    }
    return static_cast<std::uint64_t>(feedback_reader_->Poll(
        config_.feedback.poll_budget,
        [this](const OrderFeedbackEvent& event) { OnOrderFeedback(event); }));
  }

  [[nodiscard]] std::uint64_t PollDataReader() noexcept {
    if (!data_reader_) {
      return 0;
    }
    if constexpr (market_data::FiniteDataReader<DataReaderT> &&
                  requires(DataReaderT& reader, StrategyRuntime& runtime,
                           std::uint64_t max_events) {
                    reader.Drain(runtime, max_events);
                  }) {
      return static_cast<std::uint64_t>(
          data_reader_->Drain(*this, data_reader_poll_budget_));
    }
    return static_cast<std::uint64_t>(data_reader_->Poll(*this));
  }

  void CallOnStart() noexcept {
    if constexpr (requires(UserStrategyT& strategy, ContextT& context) {
                    strategy.OnStart(context);
                  }) {
      user_strategy_->OnStart(*context_);
    }
  }

  void CallOnIdle() noexcept {
    if constexpr (requires(UserStrategyT& strategy, ContextT& context) {
                    strategy.OnIdle(context);
                  }) {
      user_strategy_->OnIdle(*context_);
    }
  }

  void CallOnLoop() noexcept {
    if constexpr (requires(UserStrategyT& strategy, ContextT& context) {
                    strategy.OnLoop(context);
                  }) {
      user_strategy_->OnLoop(*context_);
    }
  }

  void CallOnStop() noexcept {
    if constexpr (requires(UserStrategyT& strategy, ContextT& context) {
                    strategy.OnStop(context);
                  }) {
      user_strategy_->OnStop(*context_);
    }
  }

  [[nodiscard]] bool ShouldStop() noexcept {
    if constexpr (requires(UserStrategyT& strategy) {
                    strategy.ShouldStop();
                  }) {
      return static_cast<bool>(user_strategy_->ShouldStop());
    }
    return false;
  }

  [[nodiscard]] bool MaxLoopSecondsElapsed(
      std::chrono::steady_clock::time_point loop_started_at) const noexcept {
    if (config_.loop.max_loop_seconds == 0) {
      return false;
    }
    return std::chrono::steady_clock::now() - loop_started_at >=
           std::chrono::seconds(config_.loop.max_loop_seconds);
  }

  config::StrategyConfig config_;
  std::optional<DataReaderT> data_reader_;
  std::uint64_t data_reader_poll_budget_{1};
  std::optional<OrderSessionT> order_session_;
  std::optional<OrderManagerT> order_manager_;
  std::optional<ContextT> context_;
  std::optional<OrderFeedbackShmManager> feedback_shm_manager_;
  std::optional<OrderFeedbackShmReader> feedback_reader_;
  std::optional<UserStrategyT> user_strategy_;
  std::chrono::steady_clock::time_point hook_loop_started_at_{};
  bool stop_order_session_requested_{false};
  bool hook_user_started_{false};
  int hook_exit_code_{0};
};

}  // namespace aquila::strategy

#endif  // AQUILA_CORE_STRATEGY_STRATEGY_RUNTIME_H_
