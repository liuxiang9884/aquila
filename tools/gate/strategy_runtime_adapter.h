#ifndef AQUILA_TOOLS_GATE_STRATEGY_RUNTIME_ADAPTER_H_
#define AQUILA_TOOLS_GATE_STRATEGY_RUNTIME_ADAPTER_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "core/strategy/order_types.h"
#include "core/websocket/types.h"
#include "exchange/gate/trading/order_session.h"
#include "exchange/gate/trading/order_session_config.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::tools::gate_strategy_runtime {

[[nodiscard]] inline strategy::OrderResponseKind ToStrategyOrderResponseKind(
    gate::OrderResponseKind kind) noexcept {
  switch (kind) {
    case gate::OrderResponseKind::kAck:
      return strategy::OrderResponseKind::kAck;
    case gate::OrderResponseKind::kAccepted:
      return strategy::OrderResponseKind::kAccepted;
    case gate::OrderResponseKind::kRejected:
      return strategy::OrderResponseKind::kRejected;
    case gate::OrderResponseKind::kCancelAccepted:
      return strategy::OrderResponseKind::kCancelAccepted;
    case gate::OrderResponseKind::kCancelRejected:
      return strategy::OrderResponseKind::kCancelRejected;
  }
  return strategy::OrderResponseKind::kRejected;
}

[[nodiscard]] inline strategy::OrderResponseEvent ToStrategyOrderResponseEvent(
    const gate::OrderResponse& response) noexcept {
  return strategy::OrderResponseEvent{
      .kind = ToStrategyOrderResponseKind(response.kind),
      .local_order_id = response.local_order_id,
      .exchange_order_id = response.exchange_order_id,
      .error_label_hash = response.error_label_hash,
  };
}

namespace detail {

class GateOrderResponseQueue {
 public:
  GateOrderResponseQueue() = default;
  GateOrderResponseQueue(const GateOrderResponseQueue&) = delete;
  GateOrderResponseQueue& operator=(const GateOrderResponseQueue&) = delete;

  void MarkReady() noexcept {
    ready_.store(true, std::memory_order_release);
  }

  void ClearReady() noexcept {
    ready_.store(false, std::memory_order_release);
  }

  [[nodiscard]] bool Ready() const noexcept {
    return ready_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool Push(const gate::OrderResponse& response) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      responses_.push_back(ToStrategyOrderResponseEvent(response));
      return true;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] bool Pop(strategy::OrderResponseEvent* event) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (responses_.empty()) {
      return false;
    }
    *event = responses_.front();
    responses_.pop_front();
    return true;
  }

 private:
  std::atomic<bool> ready_{false};
  std::mutex mutex_;
  std::deque<strategy::OrderResponseEvent> responses_;
};

class GateOrderSessionResponseHandler {
 public:
  explicit GateOrderSessionResponseHandler(
      GateOrderResponseQueue& responses) noexcept
      : responses_(responses) {}

  GateOrderSessionResponseHandler(const GateOrderSessionResponseHandler&) =
      delete;
  GateOrderSessionResponseHandler& operator=(
      const GateOrderSessionResponseHandler&) = delete;

  void OnOrderSessionLoginReady() noexcept {
    responses_.MarkReady();
  }

  void OnOrderSessionLoginNotReady() noexcept {
    responses_.ClearReady();
  }

  void OnOrderResponse(const gate::OrderResponse& response) noexcept {
    (void)responses_.Push(response);
  }

 private:
  GateOrderResponseQueue& responses_;
};

template <typename HandlerT>
void DispatchOrderResponse(HandlerT& handler,
                           const strategy::OrderResponseEvent& event) {
  if constexpr (requires { handler.OnOrderResponse(event); }) {
    handler.OnOrderResponse(event);
  } else {
    handler(event);
  }
}

}  // namespace detail

template <typename WebSocketPolicy =
              gate::OrderSessionDefaultTlsWebSocketPolicy,
          typename Diagnostics = gate::NoopOrderSessionDiagnostics>
class GateOrderSessionAdapter {
 public:
  using ResponseHandler = detail::GateOrderSessionResponseHandler;
  using Session =
      gate::OrderSession<ResponseHandler, WebSocketPolicy, Diagnostics>;

  GateOrderSessionAdapter(
      websocket::ConnectionConfig config, gate::LoginCredentials credentials,
      std::size_t request_map_capacity = gate::kDefaultOrderRequestMapCapacity)
      : impl_(std::make_unique<Impl>(std::move(config), std::move(credentials),
                                     request_map_capacity)) {}

  GateOrderSessionAdapter(gate::OrderSessionConfig config,
                          gate::LoginCredentials credentials)
      : GateOrderSessionAdapter(std::move(config.connection),
                                std::move(credentials),
                                config.request_map_capacity) {}

  ~GateOrderSessionAdapter() {
    Stop();
  }

  GateOrderSessionAdapter(const GateOrderSessionAdapter&) = delete;
  GateOrderSessionAdapter& operator=(const GateOrderSessionAdapter&) = delete;
  GateOrderSessionAdapter(GateOrderSessionAdapter&&) noexcept = default;
  GateOrderSessionAdapter& operator=(GateOrderSessionAdapter&&) noexcept =
      default;

  [[nodiscard]] bool Start() noexcept {
    return impl_ != nullptr && impl_->Start();
  }

  void Stop() noexcept {
    if (impl_ != nullptr) {
      impl_->Stop();
    }
  }

  [[nodiscard]] bool Ready() const noexcept {
    return impl_ != nullptr && impl_->Ready();
  }

  [[nodiscard]] bool Running() const noexcept {
    return impl_ != nullptr && impl_->Running();
  }

  template <typename OrderT>
  [[nodiscard]] gate::OrderSendResult PlaceOrder(const OrderT& order) noexcept {
    if (impl_ == nullptr) {
      return {.status = gate::OrderSendStatus::kNotActive,
              .request_sequence = 0,
              .encoded_request_id = 0};
    }
    return impl_->PlaceOrder(order);
  }

  template <typename OrderT>
  [[nodiscard]] gate::OrderSendResult CancelOrder(
      const OrderT& order) noexcept {
    if (impl_ == nullptr) {
      return {.status = gate::OrderSendStatus::kNotActive,
              .request_sequence = 0,
              .encoded_request_id = 0};
    }
    return impl_->CancelOrder(order);
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    if (impl_ != nullptr) {
      impl_->CacheExchangeOrderId(local_order_id, exchange_order_id);
    }
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    if (impl_ != nullptr) {
      impl_->ForgetExchangeOrderId(local_order_id);
    }
  }

  template <typename HandlerT>
  [[nodiscard]] std::size_t PollOrderResponses(HandlerT& handler) {
    if (impl_ == nullptr) {
      return 0;
    }
    return impl_->PollOrderResponses(handler);
  }

#if defined(AQUILA_GATE_STRATEGY_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
  void MarkLoginReadyForTest() noexcept {
    if (impl_ != nullptr) {
      impl_->MarkLoginReadyForTest();
    }
  }

  void PushOrderResponseForTest(const gate::OrderResponse& response) noexcept {
    if (impl_ != nullptr) {
      impl_->PushOrderResponseForTest(response);
    }
  }
#endif

 private:
  class Impl {
   public:
    Impl(websocket::ConnectionConfig config, gate::LoginCredentials credentials,
         std::size_t request_map_capacity)
        : response_handler_(responses_),
          session_(std::move(config), std::move(credentials), response_handler_,
                   request_map_capacity) {
      session_.SetRuntimeHook(this, &Impl::RunCommandHook);
    }

    ~Impl() {
      Stop();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool Start() noexcept {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (thread_.joinable()) {
        return true;
      }

      responses_.ClearReady();
      running_.store(true, std::memory_order_release);
      try {
        thread_ = std::thread([this]() noexcept {
          (void)session_.Start();
          running_.store(false, std::memory_order_release);
          responses_.ClearReady();
          FailPendingCommands(NotActiveResult());
        });
      } catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
      }
      return true;
    }

    void Stop() noexcept {
      std::thread thread_to_join;
      {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        running_.store(false, std::memory_order_release);
        session_.Stop();
        FailPendingCommands(NotActiveResult());
        if (thread_.joinable()) {
          thread_to_join = std::move(thread_);
        }
      }
      if (thread_to_join.joinable()) {
        if (thread_to_join.get_id() == std::this_thread::get_id()) {
          thread_to_join.detach();
        } else {
          thread_to_join.join();
        }
      }
      responses_.ClearReady();
    }

    [[nodiscard]] bool Ready() const noexcept {
      return running_.load(std::memory_order_acquire) && responses_.Ready();
    }

    [[nodiscard]] bool Running() const noexcept {
      return running_.load(std::memory_order_acquire);
    }

    template <typename OrderT>
    [[nodiscard]] gate::OrderSendResult PlaceOrder(
        const OrderT& order) noexcept {
      return SubmitOrderCommand(CommandType::kPlace, order);
    }

    template <typename OrderT>
    [[nodiscard]] gate::OrderSendResult CancelOrder(
        const OrderT& order) noexcept {
      return SubmitOrderCommand(CommandType::kCancel, order);
    }

    void CacheExchangeOrderId(std::uint64_t local_order_id,
                              std::uint64_t exchange_order_id) noexcept {
      auto command = MakeCommand(CommandType::kCacheExchangeOrderId);
      if (command == nullptr) {
        return;
      }
      command->local_order_id = local_order_id;
      command->exchange_order_id = exchange_order_id;
      (void)EnqueueCommand(std::move(command));
    }

    void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
      auto command = MakeCommand(CommandType::kForgetExchangeOrderId);
      if (command == nullptr) {
        return;
      }
      command->local_order_id = local_order_id;
      (void)EnqueueCommand(std::move(command));
    }

    template <typename HandlerT>
    [[nodiscard]] std::size_t PollOrderResponses(HandlerT& handler) {
      std::size_t count = 0;
      strategy::OrderResponseEvent event;
      while (responses_.Pop(&event)) {
        detail::DispatchOrderResponse(handler, event);
        ++count;
      }
      return count;
    }

    [[nodiscard]] static gate::OrderSendResult NotActiveResult() noexcept {
      return {.status = gate::OrderSendStatus::kNotActive,
              .request_sequence = 0,
              .encoded_request_id = 0};
    }

#if defined(AQUILA_GATE_STRATEGY_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
    void MarkLoginReadyForTest() noexcept {
      running_.store(true, std::memory_order_release);
      response_handler_.OnOrderSessionLoginReady();
    }

    void PushOrderResponseForTest(
        const gate::OrderResponse& response) noexcept {
      response_handler_.OnOrderResponse(response);
    }
#endif

   private:
    enum class CommandType : std::uint8_t {
      kPlace,
      kCancel,
      kCacheExchangeOrderId,
      kForgetExchangeOrderId,
    };

    struct OrderSnapshot {
      std::uint64_t local_order_id{0};
      std::uint64_t exchange_order_id{0};
      Exchange exchange{Exchange::kGate};
      std::int32_t symbol_id{0};
      std::string symbol;
      OrderSide side{OrderSide::kBuy};
      TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
      std::int64_t quantity{0};
      std::string price_text;
      bool reduce_only{false};
    };

    struct Command {
      explicit Command(CommandType command_type) noexcept
          : type(command_type) {}

      CommandType type;
      OrderSnapshot order;
      std::uint64_t local_order_id{0};
      std::uint64_t exchange_order_id{0};
      gate::OrderSendResult result{NotActiveResult()};
      bool completed{false};
      std::mutex mutex;
      std::condition_variable cv;
    };

    template <typename OrderT>
    [[nodiscard]] static OrderSnapshot SnapshotOrder(const OrderT& order) {
      OrderSnapshot snapshot;
      snapshot.local_order_id = order.local_order_id;
      if constexpr (requires { order.exchange_order_id; }) {
        snapshot.exchange_order_id = order.exchange_order_id;
      }
      if constexpr (requires { order.exchange; }) {
        snapshot.exchange = order.exchange;
      }
      snapshot.symbol_id = order.symbol_id;
      snapshot.symbol = std::string(order.symbol);
      snapshot.side = order.side;
      snapshot.time_in_force = order.time_in_force;
      snapshot.quantity = order.quantity;
      snapshot.price_text = std::string(order.price_text);
      snapshot.reduce_only = order.reduce_only;
      return snapshot;
    }

    [[nodiscard]] static gate::OrderSendResult NotLoggedInResult() noexcept {
      return {.status = gate::OrderSendStatus::kNotLoggedIn,
              .request_sequence = 0,
              .encoded_request_id = 0};
    }

    [[nodiscard]] static gate::OrderSendResult QueueFullResult() noexcept {
      return {.status = gate::OrderSendStatus::kInflightFull,
              .request_sequence = 0,
              .encoded_request_id = 0};
    }

    [[nodiscard]] static std::shared_ptr<Command> MakeCommand(
        CommandType type) noexcept {
      try {
        return std::make_shared<Command>(type);
      } catch (...) {
        return nullptr;
      }
    }

    template <typename OrderT>
    [[nodiscard]] gate::OrderSendResult SubmitOrderCommand(
        CommandType type, const OrderT& order) noexcept {
      if (!running_.load(std::memory_order_acquire)) {
        return NotActiveResult();
      }
      if (!responses_.Ready()) {
        return NotLoggedInResult();
      }

      auto command = MakeCommand(type);
      if (command == nullptr) {
        return QueueFullResult();
      }
      try {
        command->order = SnapshotOrder(order);
      } catch (...) {
        return QueueFullResult();
      }
      if (!EnqueueCommand(command)) {
        return QueueFullResult();
      }

      std::unique_lock<std::mutex> lock(command->mutex);
      command->cv.wait(lock, [&command] { return command->completed; });
      return command->result;
    }

    [[nodiscard]] bool EnqueueCommand(
        std::shared_ptr<Command> command) noexcept {
      if (!running_.load(std::memory_order_acquire)) {
        return false;
      }
      try {
        {
          std::lock_guard<std::mutex> lock(command_mutex_);
          commands_.push_back(std::move(command));
        }
        session_.Wakeup();
        return true;
      } catch (...) {
        return false;
      }
    }

    static void RunCommandHook(void* context) noexcept {
      static_cast<Impl*>(context)->ProcessCommands();
    }

    void ProcessCommands() noexcept {
      for (;;) {
        std::shared_ptr<Command> command;
        {
          std::lock_guard<std::mutex> lock(command_mutex_);
          if (commands_.empty()) {
            return;
          }
          command = std::move(commands_.front());
          commands_.pop_front();
        }
        ExecuteCommand(*command);
      }
    }

    void ExecuteCommand(Command& command) noexcept {
      switch (command.type) {
        case CommandType::kPlace:
          CompleteCommand(command, session_.PlaceOrder(command.order));
          return;
        case CommandType::kCancel:
          CompleteCommand(command, session_.CancelOrder(command.order));
          return;
        case CommandType::kCacheExchangeOrderId:
          session_.CacheExchangeOrderId(command.local_order_id,
                                        command.exchange_order_id);
          CompleteCommand(command, gate::OrderSendResult{
                                       .status = gate::OrderSendStatus::kOk});
          return;
        case CommandType::kForgetExchangeOrderId:
          session_.ForgetExchangeOrderId(command.local_order_id);
          CompleteCommand(command, gate::OrderSendResult{
                                       .status = gate::OrderSendStatus::kOk});
          return;
      }
    }

    void CompleteCommand(Command& command,
                         gate::OrderSendResult result) noexcept {
      {
        std::lock_guard<std::mutex> lock(command.mutex);
        command.result = result;
        command.completed = true;
      }
      command.cv.notify_one();
    }

    void FailPendingCommands(gate::OrderSendResult result) noexcept {
      std::deque<std::shared_ptr<Command>> pending;
      {
        std::lock_guard<std::mutex> lock(command_mutex_);
        pending.swap(commands_);
      }
      for (std::shared_ptr<Command>& command : pending) {
        if (command != nullptr) {
          CompleteCommand(*command, result);
        }
      }
    }

    detail::GateOrderResponseQueue responses_;
    ResponseHandler response_handler_;
    Session session_;
    std::atomic<bool> running_{false};
    mutable std::mutex lifecycle_mutex_;
    std::mutex command_mutex_;
    std::deque<std::shared_ptr<Command>> commands_;
    std::thread thread_;
  };

  std::unique_ptr<Impl> impl_;
};

}  // namespace aquila::tools::gate_strategy_runtime

#endif  // AQUILA_TOOLS_GATE_STRATEGY_RUNTIME_ADAPTER_H_
