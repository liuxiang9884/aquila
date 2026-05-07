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

inline constexpr std::size_t kDefaultOrderInflightCapacity = 16384;

struct LoginCredentials {
  std::string api_key;
  std::string api_secret;
};

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

  OrderSession(websocket::ConnectionConfig config, LoginCredentials credentials,
               ResponseHandler& response_handler)
      : connection_(ApplyOptions(std::move(config))),
        credentials_(std::move(credentials)),
        response_handler_(response_handler),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(connection_, message_handler_) {
    request_id_to_local_order_id_.reserve(kDefaultOrderInflightCapacity);
    client_.SetStateHook(this, &HandleState);
  }

  bool Start() noexcept {
    return client_.Start();
  }

  void Stop() noexcept {
    client_.Stop();
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
    }
  }

  OrderSendResult PlaceOrder(const PlaceOrderRequest& request) noexcept {
    if (!active_) {
      return EarlyLocalReject(OrderSendStatus::kNotActive, true);
    }
    if (!login_ready_) {
      return EarlyLocalReject(OrderSendStatus::kNotLoggedIn, false);
    }
    if (request.wire.local_order_id <= 0) {
      return EarlyLocalReject(OrderSendStatus::kInvalidLocalOrderId, true);
    }
    if (request_id_to_local_order_id_.size() >= kDefaultOrderInflightCapacity) {
      return EarlyLocalReject(OrderSendStatus::kInflightFull, true);
    }

    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, sequence);
    std::array<char, kPlaceOrderRequestBufferSize> buffer;
    const EncodedTextRequest encoded = EncodePlaceOrderRequest(
        PlaceOrderEncodeFields{.timestamp = NowSeconds(),
                               .encoded_request_id = encoded_request_id,
                               .wire = request.wire},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return SendFailure(MapEncodeStatus(encoded.status), sequence,
                         encoded_request_id);
    }

    const OrderSendStatus status = MapSendStatus(SendText(encoded.text));
    if (status != OrderSendStatus::kOk) {
      return SendFailure(status, sequence, encoded_request_id);
    }
    request_id_to_local_order_id_.emplace(sequence,
                                          request.wire.local_order_id);
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordPlaceSent();
    }
    return {.status = OrderSendStatus::kOk,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id};
  }

  OrderSendResult CancelOrder(const CancelOrderRequest& request) noexcept {
    if (!active_) {
      return EarlyLocalReject(OrderSendStatus::kNotActive, true);
    }
    if (!login_ready_) {
      return EarlyLocalReject(OrderSendStatus::kNotLoggedIn, false);
    }
    if (request.local_order_id <= 0) {
      return EarlyLocalReject(OrderSendStatus::kInvalidLocalOrderId, true);
    }
    if (request_id_to_local_order_id_.size() >= kDefaultOrderInflightCapacity) {
      return EarlyLocalReject(OrderSendStatus::kInflightFull, true);
    }

    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kCancelOrder, sequence);
    std::array<char, kCancelOrderRequestBufferSize> buffer;
    const EncodedTextRequest encoded = EncodeCancelOrderRequest(
        CancelOrderEncodeFields{.timestamp = NowSeconds(),
                                .encoded_request_id = encoded_request_id,
                                .local_order_id = request.local_order_id,
                                .exchange_order_id = request.exchange_order_id},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return SendFailure(MapEncodeStatus(encoded.status), sequence,
                         encoded_request_id);
    }

    const OrderSendStatus status = MapSendStatus(SendText(encoded.text));
    if (status != OrderSendStatus::kOk) {
      return SendFailure(status, sequence, encoded_request_id);
    }
    request_id_to_local_order_id_.emplace(sequence, request.local_order_id);
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
      case OrderEncodeStatus::kSignatureFailed:
        return OrderSendStatus::kSignatureFailed;
    }
    return OrderSendStatus::kEncodeBufferTooSmall;
  }

  [[nodiscard]] websocket::SendStatus SendText(
      std::string_view payload_text) noexcept {
    auto& core = client_.Core();
    const auto payload = std::as_bytes(
        std::span<const char>(payload_text.data(), payload_text.size()));
    return core.SendText(payload, websocket::WriteFlushMode::kTryFlushOne);
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

    const std::int64_t local_order_id = it->second;
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
      std::int64_t local_order_id) const noexcept {
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

  websocket::ConnectionConfig connection_;
  LoginCredentials credentials_;
  ResponseHandler& response_handler_;
  MessageHandler message_handler_;
  Client client_;
  [[no_unique_address]] Diagnostics diagnostics_{};
  simdjson::ondemand::parser text_parser_;
  absl::flat_hash_map<std::uint64_t, std::int64_t>
      request_id_to_local_order_id_;
  std::uint64_t request_sequence_{1};
  std::uint64_t login_request_sequence_{0};
  bool active_{false};
  bool login_ready_{false};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_H_
