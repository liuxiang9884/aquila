#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/trading/order_request_encoder.h"
#include "exchange/gate/trading/submit_response_parser.h"
#include <simdjson.h>

namespace aquila::gate {

struct LoginCredentials {
  std::string api_key;
  std::string api_secret;
};

template <typename OrderT>
[[nodiscard]] std::int64_t SignedOrderSizeForGate(
    const OrderT& order) noexcept {
  return order.side == OrderSide::kBuy ? order.quantity : -order.quantity;
}

class NoopOrderSessionDiagnostics {
 public:
  static constexpr bool kEnabled = false;

  [[nodiscard]] const OrderSessionStats& stats() const noexcept {
    return kStats;
  }

 private:
  inline static constexpr OrderSessionStats kStats{};
};

class OrderSessionDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  void RecordTextMessage() noexcept {
    ++stats_.text_messages;
  }
  void RecordParseError() noexcept {
    ++stats_.parse_errors;
  }
  void RecordIgnoredMessage() noexcept {
    ++stats_.ignored_messages;
  }
  void RecordLoginSent() noexcept {
    ++stats_.login_sent;
  }
  void RecordLoginAccepted() noexcept {
    ++stats_.login_accepted;
  }
  void RecordLoginRejected() noexcept {
    ++stats_.login_rejected;
  }
  void RecordPlaceSent() noexcept {
    ++stats_.place_sent;
  }
  void RecordCancelSent() noexcept {
    ++stats_.cancel_sent;
  }
  void RecordResponse() noexcept {
    ++stats_.responses;
  }
  void RecordUnknownRequestId() noexcept {
    ++stats_.unknown_request_ids;
  }
  void RecordLocalSendFailure() noexcept {
    ++stats_.local_send_failures;
  }

  [[nodiscard]] const OrderSessionStats& stats() const noexcept {
    return stats_;
  }

 private:
  OrderSessionStats stats_{};
};

struct OrderSessionDefaultTlsWebSocketPolicy
    : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::TlsSocket;
};

struct OrderSessionDefaultPlainWebSocketPolicy
    : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::PlainSocket;
};

template <typename ResponseHandler,
          typename WebSocketPolicy = OrderSessionDefaultTlsWebSocketPolicy,
          typename Diagnostics = NoopOrderSessionDiagnostics>
class OrderSession {
 public:
  using TransportSocket = typename WebSocketPolicy::TransportSocket;
  using MessageHandler = websocket::MessageHandlerRef<OrderSession>;
  using Client =
      websocket::BasicWebSocketClient<TransportSocket, MessageHandler>;
  static constexpr bool DiagnosticsEnabled = Diagnostics::kEnabled;
  static constexpr websocket::ClockSource kClockSource =
      WebSocketPolicy::kClockSource;

  OrderSession(
      websocket::ConnectionConfig config, LoginCredentials credentials,
      ResponseHandler& response_handler,
      std::size_t request_map_capacity = kDefaultOrderRequestMapCapacity)
      : connection_(ApplyOptions(std::move(config))),
        credentials_(std::move(credentials)),
        response_handler_(response_handler),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(connection_, message_handler_),
        request_map_capacity_(request_map_capacity == 0
                                  ? kDefaultOrderRequestMapCapacity
                                  : request_map_capacity) {
    request_id_to_local_order_id_.reserve(request_map_capacity_);
    local_order_id_to_exchange_order_id_.reserve(request_map_capacity_);
    client_.SetStateHook(this, &HandleState);
  }

  bool Start() noexcept {
    return client_.Start();
  }

  void Stop() noexcept {
    client_.Stop();
  }

  void Wakeup() noexcept {
    client_.Wakeup();
  }

  void SetRuntimeHook(void* context, websocket::RuntimeHook handler) noexcept {
    client_.SetRuntimeHook(context, handler);
  }

  websocket::DeliveryResult Handle(
      const websocket::MessageView& view) noexcept {
    if (view.kind != websocket::PayloadKind::kText) {
      return websocket::DeliveryResult::kAccepted;
    }
    return HandleText(view);
  }

  void OnConnectionPhase(websocket::ConnectionPhase phase) noexcept {
    if (phase == websocket::ConnectionPhase::kActive) {
      active_ = true;
      (void)SendLogin();
      return;
    }
    if (phase == websocket::ConnectionPhase::kDisconnected ||
        phase == websocket::ConnectionPhase::kReconnectBackoff ||
        phase == websocket::ConnectionPhase::kClosing ||
        phase == websocket::ConnectionPhase::kClosed) {
      active_ = false;
      login_ready_ = false;
      login_request_sequence_ = 0;
      request_id_to_local_order_id_.clear();
      local_order_id_to_exchange_order_id_.clear();
      NotifyLoginNotReady();
    }
  }

  template <typename OrderT>
  OrderSendResult PlaceOrder(const OrderT& order) noexcept {
    if (!active_) {
      return EarlyLocalReject(OrderSendStatus::kNotActive, true);
    }
    if (!login_ready_) {
      return EarlyLocalReject(OrderSendStatus::kNotLoggedIn, false);
    }
    if (request_id_to_local_order_id_.size() >= request_map_capacity_) {
      return EarlyLocalReject(OrderSendStatus::kInflightFull, true);
    }
    const OrderType order_type = OrderTypeForGate(order);
    if (order_type != OrderType::kLimit) {
      return EarlyLocalReject(OrderSendStatus::kUnsupportedOrderType, true);
    }

    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, sequence);
    std::array<char, kPlaceOrderRequestBufferSize> buffer;
    const EncodedTextRequest encoded = EncodePlaceOrderRequest(
        PlaceOrderEncodeFields{.timestamp = NowSeconds(),
                               .encoded_request_id = encoded_request_id,
                               .local_order_id = order.local_order_id,
                               .order_type = order_type,
                               .contract = order.symbol,
                               .signed_size = SignedOrderSizeForGate(order),
                               .price_text = order.price_text,
                               .time_in_force = order.time_in_force,
                               .reduce_only = order.reduce_only},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return SendFailure(MapEncodeStatus(encoded.status), sequence,
                         encoded_request_id);
    }

    const OrderSendStatus status = MapSendStatus(SendText(encoded.text));
    if (status != OrderSendStatus::kOk) {
      return SendFailure(status, sequence, encoded_request_id);
    }
    request_id_to_local_order_id_.emplace(sequence, order.local_order_id);
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordPlaceSent();
    }
    return {.status = OrderSendStatus::kOk,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id};
  }

  template <typename OrderT>
  OrderSendResult CancelOrder(const OrderT& order) noexcept {
    if (!active_) {
      return EarlyLocalReject(OrderSendStatus::kNotActive, true);
    }
    if (!login_ready_) {
      return EarlyLocalReject(OrderSendStatus::kNotLoggedIn, false);
    }
    if (request_id_to_local_order_id_.size() >= request_map_capacity_) {
      return EarlyLocalReject(OrderSendStatus::kInflightFull, true);
    }

    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kCancelOrder, sequence);
    std::array<char, kCancelOrderRequestBufferSize> buffer;
    const EncodedTextRequest encoded = EncodeCancelOrderRequest(
        CancelOrderEncodeFields{
            .timestamp = NowSeconds(),
            .encoded_request_id = encoded_request_id,
            .local_order_id = order.local_order_id,
            .exchange_order_id = ExchangeOrderIdForCancel(order)},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return SendFailure(MapEncodeStatus(encoded.status), sequence,
                         encoded_request_id);
    }

    const OrderSendStatus status = MapSendStatus(SendText(encoded.text));
    if (status != OrderSendStatus::kOk) {
      return SendFailure(status, sequence, encoded_request_id);
    }
    request_id_to_local_order_id_.emplace(sequence, order.local_order_id);
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordCancelSent();
    }
    return {.status = OrderSendStatus::kOk,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id};
  }

  [[nodiscard]] bool login_ready() const noexcept {
    return login_ready_;
  }

  [[nodiscard]] std::size_t inflight_count() const noexcept {
    return request_id_to_local_order_id_.size();
  }

  [[nodiscard]] std::size_t request_map_capacity() const noexcept {
    return request_map_capacity_;
  }

  [[nodiscard]] std::uint64_t exchange_order_id_for_local_order(
      std::uint64_t local_order_id) const noexcept {
    const auto it = local_order_id_to_exchange_order_id_.find(local_order_id);
    if (it == local_order_id_to_exchange_order_id_.end()) {
      return 0;
    }
    return it->second;
  }

  [[nodiscard]] bool forget_exchange_order_id_for_local_order(
      std::uint64_t local_order_id) noexcept {
    return local_order_id_to_exchange_order_id_.erase(local_order_id) != 0;
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    auto it = local_order_id_to_exchange_order_id_.find(local_order_id);
    if (it != local_order_id_to_exchange_order_id_.end()) {
      it->second = exchange_order_id;
      return;
    }
    if (local_order_id_to_exchange_order_id_.size() >= request_map_capacity_) {
      return;
    }
    local_order_id_to_exchange_order_id_.emplace(local_order_id,
                                                 exchange_order_id);
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    (void)local_order_id_to_exchange_order_id_.erase(local_order_id);
  }

  [[nodiscard]] const OrderSessionStats& stats() const noexcept {
    return diagnostics_.stats();
  }

 private:
  static void HandleState(void* context,
                          websocket::ConnectionPhase phase) noexcept {
    static_cast<OrderSession*>(context)->OnConnectionPhase(phase);
  }

  static websocket::ConnectionConfig ApplyOptions(
      websocket::ConnectionConfig config) {
    config.runtime_policy.clock_source = kClockSource;
    return config;
  }

  [[nodiscard]] std::int64_t NowSeconds() const noexcept {
    return static_cast<std::int64_t>(std::time(nullptr));
  }

  [[nodiscard]] std::uint64_t NextRequestSequence() noexcept {
    return request_sequence_++;
  }

  [[nodiscard]] OrderSendStatus MapSendStatus(
      websocket::SendStatus status) noexcept {
    switch (status) {
      case websocket::SendStatus::kOk:
        return OrderSendStatus::kOk;
      case websocket::SendStatus::kNoPreparedWriteSlot:
        return OrderSendStatus::kNoPreparedWriteSlot;
      case websocket::SendStatus::kWriteUnavailable:
        return OrderSendStatus::kWriteUnavailable;
      case websocket::SendStatus::kEncodeFailed:
      case websocket::SendStatus::kPayloadTooLarge:
        return OrderSendStatus::kEncodeBufferTooSmall;
    }
    return OrderSendStatus::kWriteUnavailable;
  }

  [[nodiscard]] OrderSendStatus MapEncodeStatus(
      OrderEncodeStatus status) noexcept {
    switch (status) {
      case OrderEncodeStatus::kOk:
        return OrderSendStatus::kOk;
      case OrderEncodeStatus::kBufferTooSmall:
        return OrderSendStatus::kEncodeBufferTooSmall;
      case OrderEncodeStatus::kInvalidOrderText:
        return OrderSendStatus::kInvalidLocalOrderId;
      case OrderEncodeStatus::kUnsupportedOrderType:
        return OrderSendStatus::kUnsupportedOrderType;
      case OrderEncodeStatus::kSignatureFailed:
        return OrderSendStatus::kSignatureFailed;
    }
    return OrderSendStatus::kEncodeBufferTooSmall;
  }

  template <typename OrderT>
  [[nodiscard]] static OrderType OrderTypeForGate(
      const OrderT& order) noexcept {
    if constexpr (requires { order.type; }) {
      return order.type;
    }
    return OrderType::kLimit;
  }

  [[nodiscard]] websocket::SendStatus SendText(
      std::string_view payload_text) noexcept {
    auto& core = client_.Core();
    const auto payload = std::as_bytes(
        std::span<const char>(payload_text.data(), payload_text.size()));
    return core.SendText(payload, websocket::WriteFlushMode::kTryFlushOne);
  }

  template <typename OrderT>
  [[nodiscard]] std::uint64_t ExchangeOrderIdForCancel(
      const OrderT& order) const noexcept {
    const std::uint64_t exchange_order_id =
        exchange_order_id_for_local_order(order.local_order_id);
    if (exchange_order_id != 0) {
      return exchange_order_id;
    }
    if constexpr (requires { order.exchange_order_id; }) {
      return order.exchange_order_id;
    }
    return 0;
  }

  [[nodiscard]] OrderSendResult SendLogin() noexcept {
    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kLogin, sequence);
    std::array<char, kLoginRequestBufferSize> buffer;
    const EncodedTextRequest encoded = EncodeLoginRequest(
        LoginRequestFields{.api_key = credentials_.api_key,
                           .api_secret = credentials_.api_secret,
                           .timestamp = NowSeconds(),
                           .encoded_request_id = encoded_request_id},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return {.status = MapEncodeStatus(encoded.status),
              .request_sequence = sequence,
              .encoded_request_id = encoded_request_id};
    }

    const OrderSendStatus status = MapSendStatus(SendText(encoded.text));
    if (status == OrderSendStatus::kOk) {
      login_request_sequence_ = sequence;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginSent();
      }
    }
    return {.status = status,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id};
  }

  [[nodiscard]] OrderSendResult EarlyLocalReject(OrderSendStatus status,
                                                 bool record_failure) noexcept {
    if constexpr (DiagnosticsEnabled) {
      if (record_failure) {
        diagnostics_.RecordLocalSendFailure();
      }
    }
    return {.status = status, .request_sequence = 0, .encoded_request_id = 0};
  }

  [[nodiscard]] OrderSendResult SendFailure(
      OrderSendStatus status, std::uint64_t sequence,
      std::uint64_t encoded_request_id) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordLocalSendFailure();
    }
    return {.status = status,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id};
  }

  websocket::DeliveryResult HandleText(
      const websocket::MessageView& view) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordTextMessage();
    }
    const std::string_view payload{
        reinterpret_cast<const char*>(view.payload.data()),
        view.payload.size()};

    const GateSubmitResponse parsed = ParseGateSubmitResponseForOrderSession(
        payload, view.readable_tail_bytes, text_parser_);
    if (parsed.parse_status != GateSubmitParseStatus::kOk ||
        !parsed.request_id.ok) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseError();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    if (parsed.request_id.type == OrderRequestType::kLogin) {
      HandleLoginResponse(parsed);
      return websocket::DeliveryResult::kAccepted;
    }

    auto it = request_id_to_local_order_id_.find(parsed.request_id.sequence);
    if (it == request_id_to_local_order_id_.end()) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordUnknownRequestId();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    const std::uint64_t local_order_id = it->second;
    if (!RequestTypeMatchesChannel(parsed)) {
      RecordIgnoredMessage();
      return websocket::DeliveryResult::kAccepted;
    }
    if (parsed.kind == GateSubmitResponseKind::kUnknown) {
      RecordIgnoredMessage();
      return websocket::DeliveryResult::kAccepted;
    }
    if (!ResponseStatusMatchesKind(parsed)) {
      RecordIgnoredMessage();
      return websocket::DeliveryResult::kAccepted;
    }
    if (parsed.kind == GateSubmitResponseKind::kAck) {
      if (!AckReqIdMatchesRequestId(parsed)) {
        RecordIgnoredMessage();
        return websocket::DeliveryResult::kAccepted;
      }
      response_handler_.OnOrderResponse(
          OrderResponse{.kind = OrderResponseKind::kAck,
                        .local_order_id = local_order_id,
                        .exchange_order_id = 0,
                        .request_sequence = parsed.request_id.sequence,
                        .http_status = parsed.http_status,
                        .error_label_hash = 0});
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordResponse();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    const bool is_cancel =
        parsed.request_id.type == OrderRequestType::kCancelOrder;
    const bool is_error = parsed.kind == GateSubmitResponseKind::kError;
    if (!is_error && !FinalResultMatchesLocalOrder(parsed, local_order_id)) {
      request_id_to_local_order_id_.erase(it);
      RecordIgnoredMessage();
      return websocket::DeliveryResult::kAccepted;
    }

    request_id_to_local_order_id_.erase(it);
    if (!is_error && !is_cancel && parsed.exchange_order_id != 0) {
      CacheExchangeOrderId(local_order_id, parsed.exchange_order_id);
    }
    if (!is_error && is_cancel) {
      local_order_id_to_exchange_order_id_.erase(local_order_id);
    }
    const OrderResponseKind kind =
        is_cancel ? (is_error ? OrderResponseKind::kCancelRejected
                              : OrderResponseKind::kCancelAccepted)
                  : (is_error ? OrderResponseKind::kRejected
                              : OrderResponseKind::kAccepted);
    response_handler_.OnOrderResponse(
        OrderResponse{.kind = kind,
                      .local_order_id = local_order_id,
                      .exchange_order_id = parsed.exchange_order_id,
                      .request_sequence = parsed.request_id.sequence,
                      .http_status = parsed.http_status,
                      .error_label_hash = parsed.error_label_hash});
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordResponse();
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void HandleLoginResponse(const GateSubmitResponse& parsed) noexcept {
    if (login_request_sequence_ == 0 ||
        parsed.request_id.sequence != login_request_sequence_) {
      RecordIgnoredMessage();
      return;
    }
    login_request_sequence_ = 0;
    if (parsed.channel != kFuturesLogin ||
        parsed.kind == GateSubmitResponseKind::kUnknown) {
      login_ready_ = false;
      RecordIgnoredMessage();
      return;
    }
    if (parsed.http_status == 200 &&
        parsed.kind == GateSubmitResponseKind::kResult &&
        parsed.has_login_uid) {
      login_ready_ = true;
      NotifyLoginReady();
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginAccepted();
      }
      return;
    }
    login_ready_ = false;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordLoginRejected();
    }
  }

  [[nodiscard]] bool RequestTypeMatchesChannel(
      const GateSubmitResponse& parsed) const noexcept {
    if (parsed.request_id.type == OrderRequestType::kPlaceOrder) {
      return parsed.channel == kFuturesOrderPlace;
    }
    if (parsed.request_id.type == OrderRequestType::kCancelOrder) {
      return parsed.channel == kFuturesOrderCancel;
    }
    return false;
  }

  [[nodiscard]] bool AckReqIdMatchesRequestId(
      const GateSubmitResponse& parsed) const noexcept {
    return parsed.has_req_id && parsed.req_id.ok &&
           parsed.req_id.type == parsed.request_id.type &&
           parsed.req_id.sequence == parsed.request_id.sequence;
  }

  [[nodiscard]] bool ResponseStatusMatchesKind(
      const GateSubmitResponse& parsed) const noexcept {
    if (parsed.kind == GateSubmitResponseKind::kAck ||
        parsed.kind == GateSubmitResponseKind::kResult) {
      return parsed.http_status == 200;
    }
    return true;
  }

  [[nodiscard]] bool FinalResultMatchesLocalOrder(
      const GateSubmitResponse& parsed,
      std::uint64_t local_order_id) const noexcept {
    if (parsed.kind != GateSubmitResponseKind::kResult) {
      return false;
    }
    if (parsed.request_id.type == OrderRequestType::kPlaceOrder) {
      return parsed.exchange_order_id != 0 && parsed.has_local_order_id &&
             parsed.local_order_id == local_order_id;
    }
    if (parsed.request_id.type == OrderRequestType::kCancelOrder) {
      if (parsed.has_local_order_id) {
        return parsed.local_order_id == local_order_id;
      }
      return parsed.exchange_order_id != 0;
    }
    return false;
  }

  void RecordIgnoredMessage() noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordIgnoredMessage();
    }
  }

  void NotifyLoginReady() noexcept {
    if constexpr (requires { response_handler_.OnOrderSessionLoginReady(); }) {
      response_handler_.OnOrderSessionLoginReady();
    }
  }

  void NotifyLoginNotReady() noexcept {
    if constexpr (requires {
                    response_handler_.OnOrderSessionLoginNotReady();
                  }) {
      response_handler_.OnOrderSessionLoginNotReady();
    }
  }

  websocket::ConnectionConfig connection_;
  LoginCredentials credentials_;
  ResponseHandler& response_handler_;
  MessageHandler message_handler_;
  Client client_;
  [[no_unique_address]] Diagnostics diagnostics_{};
  simdjson::ondemand::parser text_parser_;
  absl::flat_hash_map<std::uint64_t, std::uint64_t>
      request_id_to_local_order_id_;
  absl::flat_hash_map<std::uint64_t, std::uint64_t>
      local_order_id_to_exchange_order_id_;
  std::size_t request_map_capacity_{kDefaultOrderRequestMapCapacity};
  std::uint64_t request_sequence_{1};
  std::uint64_t login_request_sequence_{0};
  bool active_{false};
  bool login_ready_{false};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_H_
