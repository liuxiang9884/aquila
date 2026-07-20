#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_RUNTIME_ADAPTER_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_RUNTIME_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "core/trading/order_latency.h"
#include "core/trading/order_types.h"
#include "core/websocket/types.h"
#include "exchange/gate/trading/order_session.h"
#include "exchange/gate/trading/order_session_config.h"
#include "exchange/gate/trading/order_types.h"
#include "nova/utils/log.h"

namespace aquila::gate {

[[nodiscard]] inline core::OrderResponseKind ToCoreOrderResponseKind(
    gate::OrderResponseKind kind) noexcept {
  switch (kind) {
    case gate::OrderResponseKind::kAck:
      return core::OrderResponseKind::kAck;
    case gate::OrderResponseKind::kAccepted:
      return core::OrderResponseKind::kAccepted;
    case gate::OrderResponseKind::kRejected:
      return core::OrderResponseKind::kRejected;
    case gate::OrderResponseKind::kCancelAccepted:
      return core::OrderResponseKind::kCancelAccepted;
    case gate::OrderResponseKind::kCancelRejected:
      return core::OrderResponseKind::kCancelRejected;
  }
  return core::OrderResponseKind::kRejected;
}

[[nodiscard]] inline bool IsGateUnknownResultResponse(
    const gate::OrderResponse& response) noexcept {
  if (response.http_status < 500) {
    return false;
  }
  return response.kind == gate::OrderResponseKind::kRejected ||
         response.kind == gate::OrderResponseKind::kCancelRejected;
}

[[nodiscard]] inline core::OrderResponseKind ToCoreOrderResponseKind(
    const gate::OrderResponse& response) noexcept {
  if (IsGateUnknownResultResponse(response)) {
    return core::OrderResponseKind::kUnknownResult;
  }
  return ToCoreOrderResponseKind(response.kind);
}

[[nodiscard]] inline core::OrderResponseEvent ToCoreOrderResponseEvent(
    const gate::OrderResponse& response) noexcept {
  return core::OrderResponseEvent{
      .kind = ToCoreOrderResponseKind(response),
      .local_order_id = response.local_order_id,
      .parent_id = response.parent_id,
      .exchange_order_id = response.exchange_order_id,
      .route_id = response.route_id,
      .local_receive_ns = response.local_receive_ns,
      .exchange_ns = response.exchange_ns,
  };
}

namespace detail {

[[nodiscard]] inline bool IsGateErrorResponse(
    gate::OrderResponseKind kind) noexcept {
  return kind == gate::OrderResponseKind::kRejected ||
         kind == gate::OrderResponseKind::kCancelRejected;
}

#if defined(AQUILA_GATE_ORDER_SESSION_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
struct OrderSessionRuntimeErrorResponseLogRecordForTest {
  gate::OrderResponseKind kind{gate::OrderResponseKind::kRejected};
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t request_sequence{0};
  std::uint16_t http_status{0};
  std::uint64_t error_label_hash{0};
  std::int64_t local_receive_ns{0};
  std::int64_t exchange_ns{0};
  std::int64_t exchange_to_local_ns{0};
};

using OrderSessionRuntimeErrorResponseLogObserverForTest = void (*)(
    const OrderSessionRuntimeErrorResponseLogRecordForTest& record) noexcept;

[[nodiscard]] inline OrderSessionRuntimeErrorResponseLogObserverForTest&
OrderSessionRuntimeErrorResponseLogObserverSlotForTest() noexcept {
  static OrderSessionRuntimeErrorResponseLogObserverForTest observer = nullptr;
  return observer;
}

inline void SetOrderSessionRuntimeErrorResponseLogObserverForTest(
    OrderSessionRuntimeErrorResponseLogObserverForTest observer) noexcept {
  OrderSessionRuntimeErrorResponseLogObserverSlotForTest() = observer;
}

inline void NotifyOrderSessionRuntimeErrorResponseLogObserverForTest(
    const gate::OrderResponse& response) noexcept {
  OrderSessionRuntimeErrorResponseLogObserverForTest observer =
      OrderSessionRuntimeErrorResponseLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(OrderSessionRuntimeErrorResponseLogRecordForTest{
      .kind = response.kind,
      .local_order_id = response.local_order_id,
      .exchange_order_id = response.exchange_order_id,
      .request_sequence = response.request_sequence,
      .http_status = response.http_status,
      .error_label_hash = response.error_label_hash,
      .local_receive_ns = response.local_receive_ns,
      .exchange_ns = response.exchange_ns,
      .exchange_to_local_ns =
          core::LatencyDeltaNs(response.local_receive_ns, response.exchange_ns),
  });
}
#endif

inline void LogGateErrorResponse(const gate::OrderResponse& response) noexcept {
  if (!IsGateErrorResponse(response.kind)) {
    return;
  }
  if (::nova::kLogManager.logger() != nullptr) {
    const std::int64_t exchange_to_local_ns =
        core::LatencyDeltaNs(response.local_receive_ns, response.exchange_ns);
    NOVA_WARNING(
        "gate_order_response_error kind={} local_order_id={} "
        "exchange_order_id={} request_sequence={} http_status={} "
        "error_label_hash={} local_receive_ns={} exchange_ns={} "
        "exchange_to_local_ns={}",
        magic_enum::enum_name(response.kind), response.local_order_id,
        response.exchange_order_id, response.request_sequence,
        response.http_status, response.error_label_hash,
        response.local_receive_ns, response.exchange_ns, exchange_to_local_ns);
  }
#if defined(AQUILA_GATE_ORDER_SESSION_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
  NotifyOrderSessionRuntimeErrorResponseLogObserverForTest(response);
#endif
}

class OrderSessionRuntimeResponseHandler {
 public:
  using RuntimeDispatch =
      void (*)(void* context, const gate::OrderResponse& response) noexcept;

  OrderSessionRuntimeResponseHandler() = default;

  OrderSessionRuntimeResponseHandler(
      const OrderSessionRuntimeResponseHandler&) = delete;
  OrderSessionRuntimeResponseHandler& operator=(
      const OrderSessionRuntimeResponseHandler&) = delete;

  template <typename RuntimeT>
  void BindRuntime(RuntimeT& runtime) noexcept {
    runtime_context_ = &runtime;
    runtime_dispatch_ = [](void* context,
                           const gate::OrderResponse& response) noexcept {
      LogGateErrorResponse(response);
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
class OrderSessionRuntimeAdapter {
 public:
  using ResponseHandler = detail::OrderSessionRuntimeResponseHandler;
  using Session =
      gate::OrderSession<ResponseHandler, WebSocketPolicy, Diagnostics>;

  OrderSessionRuntimeAdapter(
      websocket::ConnectionConfig config, gate::LoginCredentials credentials,
      std::size_t request_map_capacity = gate::kDefaultOrderRequestMapCapacity,
      OrderSessionSocketDiagnosticsConfig socket_diagnostics_config = {})
      : impl_(std::make_unique<Impl>(std::move(config), std::move(credentials),
                                     request_map_capacity,
                                     socket_diagnostics_config)) {}

  OrderSessionRuntimeAdapter(gate::OrderSessionConfig config,
                             gate::LoginCredentials credentials)
      : OrderSessionRuntimeAdapter(
            std::move(config.connection), std::move(credentials),
            config.request_map_capacity,
            OrderSessionSocketDiagnosticsConfig{
                .enable_tcp_info = config.enable_tcp_info_diagnostics,
                .ack_latency = config.ack_latency_diagnostics}) {}

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

  [[nodiscard]] gate::OrderSendResult PlaceOrder(
      const core::OrderPlaceRequest& request) noexcept {
    if (impl_ == nullptr) {
      return {.status = gate::OrderSendStatus::kNotActive,
              .request_sequence = 0,
              .encoded_request_id = 0};
    }
    return impl_->PlaceOrder(request);
  }

  [[nodiscard]] gate::OrderSendResult CancelOrder(
      const core::OrderCancelRequest& request) noexcept {
    if (impl_ == nullptr) {
      return {.status = gate::OrderSendStatus::kNotActive,
              .request_sequence = 0,
              .encoded_request_id = 0};
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

#if defined(AQUILA_GATE_ORDER_SESSION_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
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
         std::size_t request_map_capacity,
         OrderSessionSocketDiagnosticsConfig socket_diagnostics_config)
        : session_(std::move(config), std::move(credentials), response_handler_,
                   request_map_capacity, socket_diagnostics_config) {}

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

    [[nodiscard]] gate::OrderSendResult PlaceOrder(
        const core::OrderPlaceRequest& request) noexcept {
      return session_.PlaceOrder(request);
    }

    [[nodiscard]] gate::OrderSendResult CancelOrder(
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

#if defined(AQUILA_GATE_ORDER_SESSION_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
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

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_RUNTIME_ADAPTER_H_
