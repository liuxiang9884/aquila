#ifndef AQUILA_TOOLS_GATE_STRATEGY_RUNTIME_ADAPTER_H_
#define AQUILA_TOOLS_GATE_STRATEGY_RUNTIME_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
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

class GateOrderSessionResponseHandler {
 public:
  using RuntimeDispatch =
      void (*)(void* context, const gate::OrderResponse& response) noexcept;

  GateOrderSessionResponseHandler() = default;

  GateOrderSessionResponseHandler(const GateOrderSessionResponseHandler&) =
      delete;
  GateOrderSessionResponseHandler& operator=(
      const GateOrderSessionResponseHandler&) = delete;

  template <typename RuntimeT>
  void BindRuntime(RuntimeT& runtime) noexcept {
    runtime_context_ = &runtime;
    runtime_dispatch_ = [](void* context,
                           const gate::OrderResponse& response) noexcept {
      static_cast<RuntimeT*>(context)->OnOrderResponse(
          ToStrategyOrderResponseEvent(response));
    };
  }

  void OnOrderSessionLoginReady() noexcept {
    ready_ = true;
  }

  void OnOrderSessionLoginNotReady() noexcept {
    ready_ = false;
  }

  void OnOrderResponse(const gate::OrderResponse& response) noexcept {
    if (runtime_dispatch_ != nullptr) {
      runtime_dispatch_(runtime_context_, response);
    }
  }

  [[nodiscard]] bool Ready() const noexcept {
    return ready_;
  }

 private:
  bool ready_{false};
  void* runtime_context_{nullptr};
  RuntimeDispatch runtime_dispatch_{nullptr};
};

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

  template <typename RuntimeT>
  void BindRuntime(RuntimeT& runtime) noexcept {
    if (impl_ != nullptr) {
      impl_->BindRuntime(runtime);
    }
  }

  void SetRuntimeHook(void* context, websocket::RuntimeHook hook) noexcept {
    if (impl_ != nullptr) {
      impl_->SetRuntimeHook(context, hook);
    }
  }

#if defined(AQUILA_GATE_STRATEGY_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
  void MarkLoginReadyForTest() noexcept {
    if (impl_ != nullptr) {
      impl_->MarkLoginReadyForTest();
    }
  }

  void MarkLoginNotReadyForTest() noexcept {
    if (impl_ != nullptr) {
      impl_->MarkLoginNotReadyForTest();
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
        : session_(std::move(config), std::move(credentials), response_handler_,
                   request_map_capacity) {}

    ~Impl() {
      Stop();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool Start() noexcept {
      return session_.Start();
    }

    void Stop() noexcept {
      session_.Stop();
    }

    [[nodiscard]] bool Ready() const noexcept {
      return session_.login_ready() || response_handler_.Ready();
    }

    template <typename OrderT>
    [[nodiscard]] gate::OrderSendResult PlaceOrder(
        const OrderT& order) noexcept {
      return session_.PlaceOrder(order);
    }

    template <typename OrderT>
    [[nodiscard]] gate::OrderSendResult CancelOrder(
        const OrderT& order) noexcept {
      return session_.CancelOrder(order);
    }

    void CacheExchangeOrderId(std::uint64_t local_order_id,
                              std::uint64_t exchange_order_id) noexcept {
      session_.CacheExchangeOrderId(local_order_id, exchange_order_id);
    }

    void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
      session_.ForgetExchangeOrderId(local_order_id);
    }

    template <typename RuntimeT>
    void BindRuntime(RuntimeT& runtime) noexcept {
      response_handler_.BindRuntime(runtime);
    }

    void SetRuntimeHook(void* context, websocket::RuntimeHook hook) noexcept {
      session_.SetRuntimeHook(context, hook);
    }

#if defined(AQUILA_GATE_STRATEGY_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
    void MarkLoginReadyForTest() noexcept {
      response_handler_.OnOrderSessionLoginReady();
    }

    void MarkLoginNotReadyForTest() noexcept {
      response_handler_.OnOrderSessionLoginNotReady();
    }

    void PushOrderResponseForTest(
        const gate::OrderResponse& response) noexcept {
      response_handler_.OnOrderResponse(response);
    }
#endif

   private:
    ResponseHandler response_handler_;
    Session session_;
  };

  std::unique_ptr<Impl> impl_;
};

}  // namespace aquila::tools::gate_strategy_runtime

#endif  // AQUILA_TOOLS_GATE_STRATEGY_RUNTIME_ADAPTER_H_
