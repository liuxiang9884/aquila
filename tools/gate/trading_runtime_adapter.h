#ifndef AQUILA_TOOLS_GATE_TRADING_RUNTIME_ADAPTER_H_
#define AQUILA_TOOLS_GATE_TRADING_RUNTIME_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "core/strategy/order_types.h"
#include "core/websocket/types.h"
#include "exchange/gate/trading/order_session.h"
#include "exchange/gate/trading/order_session_config.h"
#include "exchange/gate/trading/order_types.h"
#include "nova/utils/log.h"

namespace aquila::tools::gate_trading_runtime {

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
  };
}

namespace detail {

[[nodiscard]] inline bool IsGateErrorResponse(
    gate::OrderResponseKind kind) noexcept {
  return kind == gate::OrderResponseKind::kRejected ||
         kind == gate::OrderResponseKind::kCancelRejected;
}

#if defined(AQUILA_GATE_TRADING_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
struct GateErrorResponseLogRecordForTest {
  gate::OrderResponseKind kind{gate::OrderResponseKind::kRejected};
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t request_sequence{0};
  std::uint16_t http_status{0};
  std::uint64_t error_label_hash{0};
};

using GateErrorResponseLogObserverForTest =
    void (*)(const GateErrorResponseLogRecordForTest& record) noexcept;

[[nodiscard]] inline GateErrorResponseLogObserverForTest&
GateErrorResponseLogObserverSlotForTest() noexcept {
  static GateErrorResponseLogObserverForTest observer = nullptr;
  return observer;
}

inline void SetGateErrorResponseLogObserverForTest(
    GateErrorResponseLogObserverForTest observer) noexcept {
  GateErrorResponseLogObserverSlotForTest() = observer;
}

inline void NotifyGateErrorResponseLogObserverForTest(
    const gate::OrderResponse& response) noexcept {
  GateErrorResponseLogObserverForTest observer =
      GateErrorResponseLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(GateErrorResponseLogRecordForTest{
      .kind = response.kind,
      .local_order_id = response.local_order_id,
      .exchange_order_id = response.exchange_order_id,
      .request_sequence = response.request_sequence,
      .http_status = response.http_status,
      .error_label_hash = response.error_label_hash,
  });
}
#endif

inline void LogGateErrorResponse(const gate::OrderResponse& response) noexcept {
  if (!IsGateErrorResponse(response.kind)) {
    return;
  }
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_WARNING(
        "gate_order_response_error kind={} local_order_id={} "
        "exchange_order_id={} request_sequence={} http_status={} "
        "error_label_hash={}",
        magic_enum::enum_name(response.kind), response.local_order_id,
        response.exchange_order_id, response.request_sequence,
        response.http_status, response.error_label_hash);
  }
#if defined(AQUILA_GATE_TRADING_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
  NotifyGateErrorResponseLogObserverForTest(response);
#endif
}

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
      LogGateErrorResponse(response);
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

#if defined(AQUILA_GATE_TRADING_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
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

#if defined(AQUILA_GATE_TRADING_RUNTIME_ADAPTER_ENABLE_TEST_HOOKS)
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

}  // namespace aquila::tools::gate_trading_runtime

#endif  // AQUILA_TOOLS_GATE_TRADING_RUNTIME_ADAPTER_H_
