#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SESSION_RUNTIME_ADAPTER_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SESSION_RUNTIME_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "core/trading/order_types.h"
#include "core/websocket/types.h"
#include "exchange/bitget/trading/order_session.h"
#include "exchange/bitget/trading/order_session_config.h"
#include "exchange/bitget/trading/order_types.h"
#include "nova/utils/log.h"

namespace aquila::bitget {

[[nodiscard]] inline core::OrderResponseKind ToCoreOrderResponseKind(
    OrderResponseKind kind) noexcept {
  switch (kind) {
    case OrderResponseKind::kAck:
      return core::OrderResponseKind::kAck;
    case OrderResponseKind::kRejected:
      return core::OrderResponseKind::kRejected;
    case OrderResponseKind::kCancelRejected:
      return core::OrderResponseKind::kCancelRejected;
    case OrderResponseKind::kUnknownResult:
      return core::OrderResponseKind::kUnknownResult;
  }
  return core::OrderResponseKind::kUnknownResult;
}

[[nodiscard]] inline core::OrderResponseEvent ToCoreOrderResponseEvent(
    const OrderResponse& response) noexcept {
  return core::OrderResponseEvent{
      .kind = ToCoreOrderResponseKind(response.kind),
      .local_order_id = response.local_order_id,
      .group_id = response.group_id,
      .exchange_order_id = response.exchange_order_id,
      .route_id = response.route_id,
      .local_receive_ns = response.local_receive_ns,
      .exchange_ns = response.exchange_ns,
  };
}

namespace detail {

inline void LogBitgetErrorResponse(const OrderResponse& response) noexcept {
  if (response.kind == OrderResponseKind::kAck ||
      ::nova::kLogManager.logger() == nullptr) {
    return;
  }
  NOVA_WARNING(
      "bitget_order_response_error kind={} request_type={} "
      "request_sequence={} local_order_id={} group_id={} "
      "route_id={} exchange_order_id={} "
      "error_code={} local_receive_ns={} exchange_ns={}",
      magic_enum::enum_name(response.kind),
      magic_enum::enum_name(response.request_type), response.request_sequence,
      response.local_order_id, response.group_id, response.route_id,
      response.exchange_order_id, response.error_code,
      response.local_receive_ns, response.exchange_ns);
}

class OrderSessionRuntimeResponseHandler {
 public:
  using RuntimeDispatch = void (*)(void* context,
                                   const OrderResponse& response) noexcept;

  OrderSessionRuntimeResponseHandler() = default;
  OrderSessionRuntimeResponseHandler(
      const OrderSessionRuntimeResponseHandler&) = delete;
  OrderSessionRuntimeResponseHandler& operator=(
      const OrderSessionRuntimeResponseHandler&) = delete;

  template <typename RuntimeT>
  void BindRuntime(RuntimeT& runtime) noexcept {
    runtime_context_ = &runtime;
    runtime_dispatch_ = [](void* context,
                           const OrderResponse& response) noexcept {
      LogBitgetErrorResponse(response);
      static_cast<RuntimeT*>(context)->OnOrderResponse(
          ToCoreOrderResponseEvent(response));
    };
  }

  void OnOrderSessionLoginReady() noexcept {
    ready_ = true;
  }
  void OnOrderSessionLoginNotReady() noexcept {
    ready_ = false;
  }

  void OnOrderResponse(const OrderResponse& response) noexcept {
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

template <typename WebSocketPolicy = OrderSessionDefaultTlsWebSocketPolicy,
          typename Diagnostics = NoopOrderSessionDiagnostics>
class OrderSessionRuntimeAdapter {
 public:
  using ResponseHandler = detail::OrderSessionRuntimeResponseHandler;
  using Session = OrderSession<ResponseHandler, WebSocketPolicy, Diagnostics>;

  OrderSessionRuntimeAdapter(
      websocket::ConnectionConfig config, LoginCredentials credentials,
      ClientOidRunNamespace client_oid_run_namespace,
      std::size_t request_map_capacity = kDefaultOrderRequestMapCapacity,
      std::size_t order_id_cache_capacity = kDefaultOrderIdCacheCapacity)
      : impl_(std::make_unique<Impl>(
            std::move(config), std::move(credentials), client_oid_run_namespace,
            request_map_capacity, order_id_cache_capacity)) {}

  OrderSessionRuntimeAdapter(OrderSessionConfig config,
                             LoginCredentials credentials)
      : OrderSessionRuntimeAdapter(
            std::move(config.connection), std::move(credentials),
            config.client_oid_run_namespace, config.request_map_capacity,
            config.order_id_cache_capacity) {}

  ~OrderSessionRuntimeAdapter() {
    Stop();
  }

  OrderSessionRuntimeAdapter(const OrderSessionRuntimeAdapter&) = delete;
  OrderSessionRuntimeAdapter& operator=(const OrderSessionRuntimeAdapter&) =
      delete;
  OrderSessionRuntimeAdapter(OrderSessionRuntimeAdapter&&) noexcept = default;
  OrderSessionRuntimeAdapter& operator=(OrderSessionRuntimeAdapter&&) noexcept =
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

  [[nodiscard]] OrderSendResult PlaceOrder(
      const core::OrderPlaceRequest& request) noexcept {
    if (impl_ == nullptr) {
      return {.status = OrderSendStatus::kNotActive};
    }
    return impl_->PlaceOrder(request);
  }

  [[nodiscard]] OrderSendResult CancelOrder(
      const core::OrderCancelRequest& request) noexcept {
    if (impl_ == nullptr) {
      return {.status = OrderSendStatus::kNotActive};
    }
    return impl_->CancelOrder(request);
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

#if defined(AQUILA_BITGET_ORDER_SESSION_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
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

  void PushOrderResponseForTest(const OrderResponse& response) noexcept {
    if (impl_ != nullptr) {
      impl_->PushOrderResponseForTest(response);
    }
  }
#endif

 private:
  class Impl {
   public:
    Impl(websocket::ConnectionConfig config, LoginCredentials credentials,
         ClientOidRunNamespace client_oid_run_namespace,
         std::size_t request_map_capacity, std::size_t order_id_cache_capacity)
        : session_(std::move(config), std::move(credentials),
                   client_oid_run_namespace, response_handler_,
                   request_map_capacity, order_id_cache_capacity) {}

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

    [[nodiscard]] OrderSendResult PlaceOrder(
        const core::OrderPlaceRequest& request) noexcept {
      return session_.PlaceOrder(request);
    }

    [[nodiscard]] OrderSendResult CancelOrder(
        const core::OrderCancelRequest& request) noexcept {
      return session_.CancelOrder(request);
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

#if defined(AQUILA_BITGET_ORDER_SESSION_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
    void MarkLoginReadyForTest() noexcept {
      response_handler_.OnOrderSessionLoginReady();
    }
    void MarkLoginNotReadyForTest() noexcept {
      response_handler_.OnOrderSessionLoginNotReady();
    }
    void PushOrderResponseForTest(const OrderResponse& response) noexcept {
      response_handler_.OnOrderResponse(response);
    }
#endif

   private:
    ResponseHandler response_handler_;
    Session session_;
  };

  std::unique_ptr<Impl> impl_;
};

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SESSION_RUNTIME_ADAPTER_H_
