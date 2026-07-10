#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SESSION_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SESSION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <span>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <magic_enum/magic_enum.hpp>

#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"
#include "exchange/bitget/trading/operation_response_parser.h"
#include "exchange/bitget/trading/order_request_encoder.h"
#include "exchange/bitget/trading/order_types.h"
#include "nova/utils/log.h"
#include <simdjson.h>

namespace aquila::bitget {

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
  void RecordPingSent() noexcept {
    ++stats_.pings_sent;
  }
  void RecordPongReceived() noexcept {
    ++stats_.pongs_received;
  }
  void RecordHeartbeatTimeout() noexcept {
    ++stats_.heartbeat_timeouts;
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
  void RecordCorrelationMismatch() noexcept {
    ++stats_.correlation_mismatches;
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

namespace detail {

struct OrderRequestCorrelation {
  OrderRequestType type{OrderRequestType::kUnknown};
  std::uint64_t local_order_id{0};
  std::uint64_t parent_id{0};
  std::uint64_t expected_exchange_order_id{0};
  std::int64_t request_send_local_ns{0};
  std::uint16_t route_id{static_cast<std::uint16_t>(0xFFFF)};
};

}  // namespace detail

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
      std::size_t request_map_capacity = kDefaultOrderRequestMapCapacity,
      std::size_t order_id_cache_capacity = kDefaultOrderIdCacheCapacity)
      : connection_(ApplyOptions(std::move(config))),
        credentials_(std::move(credentials)),
        response_handler_(response_handler),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(connection_, message_handler_),
        request_map_capacity_(request_map_capacity == 0
                                  ? kDefaultOrderRequestMapCapacity
                                  : request_map_capacity),
        order_id_cache_capacity_(order_id_cache_capacity == 0
                                     ? kDefaultOrderIdCacheCapacity
                                     : order_id_cache_capacity),
        application_heartbeat_interval_ns_(
            static_cast<std::uint64_t>(connection_.heartbeat_interval_ms) *
            1'000'000ULL),
        application_heartbeat_timeout_ns_(
            static_cast<std::uint64_t>(connection_.heartbeat_timeout_ms) *
            1'000'000ULL) {
    request_correlations_.reserve(request_map_capacity_);
    local_order_id_to_exchange_order_id_.reserve(order_id_cache_capacity_);
    client_.SetStateHook(this, &HandleState);
    client_.SetRuntimeLoopProbe(this, &HandleRuntimeLoopProbe);
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
      if (active_) {
        return;
      }
      active_ = true;
      login_ready_ = false;
      login_sent_ = false;
      application_awaiting_pong_ = false;
      application_last_ping_ns_ = websocket::NowNs(kClockSource);
      if (::nova::kLogManager.logger() != nullptr) {
        NOVA_INFO(
            "bitget_order_session_connected host={} port={} target={} "
            "inflight={} request_map_capacity={} order_id_cache_capacity={}",
            connection_.host, connection_.port, connection_.target,
            inflight_count(), request_map_capacity_, order_id_cache_capacity_);
      }
      const OrderSendStatus login_status = SendLogin();
      if (IsTransientControlSendFailure(login_status)) {
        RequestProtocolReconnect();
      }
      return;
    }
    if (phase == websocket::ConnectionPhase::kDisconnected ||
        phase == websocket::ConnectionPhase::kReconnectBackoff ||
        phase == websocket::ConnectionPhase::kClosing ||
        phase == websocket::ConnectionPhase::kClosed) {
      if (::nova::kLogManager.logger() != nullptr) {
        NOVA_WARNING(
            "bitget_order_session_phase phase={} active_before={} "
            "login_ready_before={} inflight_before={} last_error={} "
            "reconnect_trigger={} reconnect_errno={}",
            magic_enum::enum_name(phase), active_ ? "true" : "false",
            login_ready_ ? "true" : "false", inflight_count(),
            magic_enum::enum_name(client_.last_error()),
            magic_enum::enum_name(client_.last_reconnect_trigger()),
            client_.last_reconnect_errno());
      }
      active_ = false;
      login_ready_ = false;
      login_sent_ = false;
      application_awaiting_pong_ = false;
      application_last_ping_ns_ = 0;
      place_cache_reservations_ = 0;
      request_correlations_.clear();
      local_order_id_to_exchange_order_id_.clear();
      NotifyLoginNotReady();
    }
  }

  template <typename OrderT>
  OrderSendResult PlaceOrder(const OrderT& order) noexcept {
    const OrderSendStatus ready_status = CheckReadyForSend();
    if (ready_status != OrderSendStatus::kOk) {
      return LocalFailure(ready_status);
    }
    if (request_correlations_.size() >= request_map_capacity_) {
      return LocalFailure(OrderSendStatus::kInflightFull);
    }
    if (local_order_id_to_exchange_order_id_.find(order.local_order_id) ==
            local_order_id_to_exchange_order_id_.end() &&
        local_order_id_to_exchange_order_id_.size() +
                place_cache_reservations_ >=
            order_id_cache_capacity_) {
      return LocalFailure(OrderSendStatus::kOrderIdCacheFull);
    }

    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, sequence);
    std::array<char, kPlaceOrderRequestBufferSize> buffer{};
    const EncodedTextRequest encoded = EncodePlaceOrderRequest(
        PlaceOrderEncodeFields{
            .encoded_request_id = encoded_request_id,
            .local_order_id = order.local_order_id,
            .order_type = OrderTypeFor(order),
            .symbol = order.symbol,
            .quantity_text = order.quantity_text,
            .price_text = order.price_text,
            .side = order.side,
            .time_in_force = order.time_in_force,
            .reduce_only = order.reduce_only,
        },
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return SendFailure(MapEncodeStatus(encoded.status), sequence,
                         encoded_request_id);
    }

    const std::int64_t send_local_ns = RealtimeNowNs();
    const OrderSendStatus send_status = MapSendStatus(SendText(encoded.text));
    if (send_status != OrderSendStatus::kOk) {
      return SendFailure(send_status, sequence, encoded_request_id);
    }
    request_correlations_.emplace(
        sequence, MakeCorrelation(OrderRequestType::kPlaceOrder, order, 0,
                                  send_local_ns));
    ++place_cache_reservations_;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordPlaceSent();
    }
    LogOrderSend(OrderRequestType::kPlaceOrder, order.local_order_id, sequence,
                 send_local_ns);
    return {.status = OrderSendStatus::kOk,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id,
            .send_local_ns = send_local_ns};
  }

  template <typename OrderT>
  OrderSendResult CancelOrder(const OrderT& order) noexcept {
    const OrderSendStatus ready_status = CheckReadyForSend();
    if (ready_status != OrderSendStatus::kOk) {
      return LocalFailure(ready_status);
    }
    if (request_correlations_.size() >= request_map_capacity_) {
      return LocalFailure(OrderSendStatus::kInflightFull);
    }

    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kCancelOrder, sequence);
    const std::uint64_t exchange_order_id = ExchangeOrderIdForCancel(order);
    std::array<char, kCancelOrderRequestBufferSize> buffer{};
    const EncodedTextRequest encoded = EncodeCancelOrderRequest(
        CancelOrderEncodeFields{
            .encoded_request_id = encoded_request_id,
            .local_order_id = order.local_order_id,
            .exchange_order_id = exchange_order_id,
        },
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return SendFailure(MapEncodeStatus(encoded.status), sequence,
                         encoded_request_id);
    }

    const std::int64_t send_local_ns = RealtimeNowNs();
    const OrderSendStatus send_status = MapSendStatus(SendText(encoded.text));
    if (send_status != OrderSendStatus::kOk) {
      return SendFailure(send_status, sequence, encoded_request_id);
    }
    request_correlations_.emplace(
        sequence, MakeCorrelation(OrderRequestType::kCancelOrder, order,
                                  exchange_order_id, send_local_ns));
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordCancelSent();
    }
    LogOrderSend(OrderRequestType::kCancelOrder, order.local_order_id, sequence,
                 send_local_ns);
    return {.status = OrderSendStatus::kOk,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id,
            .send_local_ns = send_local_ns};
  }

  [[nodiscard]] bool Ready() const noexcept {
    return active_ && login_ready_;
  }
  [[nodiscard]] bool active() const noexcept {
    return active_;
  }
  [[nodiscard]] bool login_ready() const noexcept {
    return login_ready_;
  }
  [[nodiscard]] std::size_t inflight_count() const noexcept {
    return request_correlations_.size();
  }
  [[nodiscard]] std::size_t request_map_capacity() const noexcept {
    return request_map_capacity_;
  }
  [[nodiscard]] std::size_t order_id_cache_capacity() const noexcept {
    return order_id_cache_capacity_;
  }
  [[nodiscard]] const OrderSessionStats& stats() const noexcept {
    return diagnostics_.stats();
  }
  [[nodiscard]] const websocket::ConnectionConfig& connection() const noexcept {
    return connection_;
  }
  [[nodiscard]] websocket::ConnectionPhase phase() const noexcept {
    return client_.phase();
  }
  [[nodiscard]] websocket::ConnectionError last_error() const noexcept {
    return client_.last_error();
  }
  [[nodiscard]] websocket::Metrics SnapshotMetrics() const noexcept {
    return client_.SnapshotMetrics();
  }

  [[nodiscard]] std::uint64_t exchange_order_id_for_local_order(
      std::uint64_t local_order_id) const noexcept {
    const auto it = local_order_id_to_exchange_order_id_.find(local_order_id);
    return it == local_order_id_to_exchange_order_id_.end() ? 0 : it->second;
  }

  [[nodiscard]] bool forget_exchange_order_id_for_local_order(
      std::uint64_t local_order_id) noexcept {
    return local_order_id_to_exchange_order_id_.erase(local_order_id) != 0;
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    if (local_order_id == 0 || exchange_order_id == 0) {
      return;
    }
    auto it = local_order_id_to_exchange_order_id_.find(local_order_id);
    if (it != local_order_id_to_exchange_order_id_.end()) {
      it->second = exchange_order_id;
      return;
    }
    if (local_order_id_to_exchange_order_id_.size() >=
        order_id_cache_capacity_) {
      return;
    }
    local_order_id_to_exchange_order_id_.emplace(local_order_id,
                                                 exchange_order_id);
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    (void)local_order_id_to_exchange_order_id_.erase(local_order_id);
  }

#if defined(AQUILA_BITGET_ORDER_SESSION_ENABLE_TEST_HOOKS)
  void AdvanceApplicationHeartbeatForTest(std::uint64_t now_ns) noexcept {
    AdvanceApplicationHeartbeat(now_ns);
  }
  [[nodiscard]] std::uint64_t application_last_ping_ns_for_test()
      const noexcept {
    return application_last_ping_ns_;
  }
  [[nodiscard]] bool application_awaiting_pong_for_test() const noexcept {
    return application_awaiting_pong_;
  }
  [[nodiscard]] bool reconnect_requested_for_test() noexcept {
    return client_.Core().ShouldReconnect();
  }
#endif

 private:
  static void HandleState(void* context,
                          websocket::ConnectionPhase phase) noexcept {
    static_cast<OrderSession*>(context)->OnConnectionPhase(phase);
  }

  static void HandleRuntimeLoopProbe(
      void* context, websocket::RuntimeLoopProbePoint point) noexcept {
    if (point == websocket::RuntimeLoopProbePoint::kAfterRuntimeHook) {
      static_cast<OrderSession*>(context)->AdvanceApplicationHeartbeat(
          websocket::NowNs(kClockSource));
    }
  }

  static websocket::ConnectionConfig ApplyOptions(
      websocket::ConnectionConfig config) {
    config.runtime_policy.clock_source = kClockSource;
    return config;
  }

  [[nodiscard]] static std::int64_t RealtimeNowNs() noexcept {
    return static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
  }

  [[nodiscard]] std::uint64_t NextRequestSequence() noexcept {
    const std::uint64_t sequence = request_sequence_++;
    if ((request_sequence_ & RequestIdCodec::kSequenceMask) == 0) {
      request_sequence_ = 1;
    }
    return sequence & RequestIdCodec::kSequenceMask;
  }

  [[nodiscard]] OrderSendStatus CheckReadyForSend() const noexcept {
    if (!active_) {
      return OrderSendStatus::kNotActive;
    }
    if (!login_ready_) {
      return OrderSendStatus::kNotLoggedIn;
    }
    return OrderSendStatus::kOk;
  }

  [[nodiscard]] OrderSendResult LocalFailure(OrderSendStatus status) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordLocalSendFailure();
    }
    return {.status = status};
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

  [[nodiscard]] static OrderSendStatus MapSendStatus(
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

  [[nodiscard]] static OrderSendStatus MapEncodeStatus(
      OrderEncodeStatus status) noexcept {
    switch (status) {
      case OrderEncodeStatus::kOk:
        return OrderSendStatus::kOk;
      case OrderEncodeStatus::kBufferTooSmall:
        return OrderSendStatus::kEncodeBufferTooSmall;
      case OrderEncodeStatus::kSignatureFailed:
      case OrderEncodeStatus::kInvalidCredentials:
        return OrderSendStatus::kSignatureFailed;
      case OrderEncodeStatus::kInvalidClientOid:
        return OrderSendStatus::kInvalidLocalOrderId;
      case OrderEncodeStatus::kInvalidSymbol:
        return OrderSendStatus::kInvalidSymbol;
      case OrderEncodeStatus::kInvalidQuantityText:
        return OrderSendStatus::kInvalidQuantityText;
      case OrderEncodeStatus::kInvalidPriceText:
        return OrderSendStatus::kInvalidPriceText;
      case OrderEncodeStatus::kUnsupportedOrderType:
        return OrderSendStatus::kUnsupportedOrderType;
    }
    return OrderSendStatus::kEncodeBufferTooSmall;
  }

  [[nodiscard]] static bool IsTransientControlSendFailure(
      OrderSendStatus status) noexcept {
    return status == OrderSendStatus::kNoPreparedWriteSlot ||
           status == OrderSendStatus::kWriteUnavailable;
  }

  [[nodiscard]] static bool IsSessionInvalidatingError(
      std::uint32_t code) noexcept {
    return code == 30004 || code == 30007 || code == 30033;
  }

  void RequestProtocolReconnect() noexcept {
    client_.RequestReconnect(websocket::ConnectionError::kProtocolError,
                             websocket::ReconnectTrigger::kProtocolError);
  }

  [[nodiscard]] websocket::SendStatus SendText(std::string_view text) noexcept {
    return client_.Core().SendText(
        std::as_bytes(std::span<const char>(text.data(), text.size())),
        websocket::WriteFlushMode::kTryFlushOne);
  }

  [[nodiscard]] OrderSendStatus SendLogin() noexcept {
    std::array<char, kLoginRequestBufferSize> buffer{};
    const EncodedTextRequest encoded = EncodeLoginRequest(
        LoginRequestFields{
            .api_key = credentials_.api_key,
            .api_secret = credentials_.api_secret,
            .passphrase = credentials_.passphrase,
            .timestamp_seconds = static_cast<std::int64_t>(std::time(nullptr)),
        },
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return MapEncodeStatus(encoded.status);
    }
    const OrderSendStatus status = MapSendStatus(SendText(encoded.text));
    if (status == OrderSendStatus::kOk) {
      login_sent_ = true;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginSent();
      }
    }
    return status;
  }

  websocket::DeliveryResult HandleText(
      const websocket::MessageView& view) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordTextMessage();
    }
    const std::string_view payload{
        reinterpret_cast<const char*>(view.payload.data()),
        view.payload.size()};
    if (payload == "pong") {
      const bool was_awaiting_pong = application_awaiting_pong_;
      application_awaiting_pong_ = false;
      if (was_awaiting_pong) {
        if constexpr (DiagnosticsEnabled) {
          diagnostics_.RecordPongReceived();
        }
      }
      return websocket::DeliveryResult::kAccepted;
    }

    const std::int64_t local_receive_ns = RealtimeNowNs();
    const OperationResponse parsed =
        ParseOperationResponse(payload, view.readable_tail_bytes, text_parser_);
    if (parsed.parse_status != OperationParseStatus::kOk) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseError();
      }
      return websocket::DeliveryResult::kAccepted;
    }
    if (parsed.kind == OperationResponseKind::kLoginAccepted ||
        parsed.kind == OperationResponseKind::kLoginRejected) {
      HandleLoginResponse(parsed);
      return websocket::DeliveryResult::kAccepted;
    }
    if (!parsed.request_id.ok) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseError();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    auto correlation_it =
        request_correlations_.find(parsed.request_id.sequence);
    if (correlation_it == request_correlations_.end()) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordUnknownRequestId();
      }
      return websocket::DeliveryResult::kAccepted;
    }
    const detail::OrderRequestCorrelation correlation = correlation_it->second;
    if (correlation.type != parsed.request_id.type) {
      RecordCorrelationMismatch();
      return websocket::DeliveryResult::kAccepted;
    }
    if (parsed.kind == OperationResponseKind::kAck &&
        (!parsed.client_oid.ok ||
         parsed.client_oid.local_order_id != correlation.local_order_id)) {
      RecordCorrelationMismatch();
      return websocket::DeliveryResult::kAccepted;
    }
    if (parsed.kind == OperationResponseKind::kAck &&
        correlation.type == OrderRequestType::kCancelOrder &&
        correlation.expected_exchange_order_id != 0 &&
        parsed.exchange_order_id != 0 &&
        parsed.exchange_order_id != correlation.expected_exchange_order_id) {
      RecordCorrelationMismatch();
      return websocket::DeliveryResult::kAccepted;
    }

    request_correlations_.erase(correlation_it);
    if (correlation.type == OrderRequestType::kPlaceOrder &&
        place_cache_reservations_ != 0) {
      --place_cache_reservations_;
    }
    if (parsed.kind == OperationResponseKind::kAck &&
        correlation.type == OrderRequestType::kPlaceOrder &&
        parsed.exchange_order_id != 0) {
      CacheExchangeOrderId(correlation.local_order_id,
                           parsed.exchange_order_id);
    }

    const std::int64_t ack_rtt_ns =
        local_receive_ns >= correlation.request_send_local_ns
            ? local_receive_ns - correlation.request_send_local_ns
            : -1;
    const OrderResponse response{
        .kind = MapResponseKind(parsed.kind),
        .request_type = correlation.type,
        .local_order_id = correlation.local_order_id,
        .parent_id = correlation.parent_id,
        .exchange_order_id = parsed.exchange_order_id,
        .request_sequence = parsed.request_id.sequence,
        .route_id = correlation.route_id,
        .error_code = parsed.error_code,
        .connection_id_hash = parsed.connection_id_hash,
        .request_send_local_ns = correlation.request_send_local_ns,
        .local_receive_ns = local_receive_ns,
        .exchange_ns = parsed.exchange_ns,
        .ack_rtt_ns = ack_rtt_ns,
    };
    if (IsSessionInvalidatingError(parsed.error_code)) {
      login_ready_ = false;
      NotifyLoginNotReady();
      RequestProtocolReconnect();
    }
    LogOrderResponse(response);
    response_handler_.OnOrderResponse(response);
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordResponse();
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void HandleLoginResponse(const OperationResponse& response) noexcept {
    if (response.kind == OperationResponseKind::kLoginRejected &&
        IsSessionInvalidatingError(response.error_code)) {
      login_ready_ = false;
      login_sent_ = false;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginRejected();
      }
      NotifyLoginNotReady();
      RequestProtocolReconnect();
      return;
    }
    if (!active_ || !login_sent_) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordIgnoredMessage();
      }
      return;
    }
    login_sent_ = false;
    if (response.kind == OperationResponseKind::kLoginAccepted) {
      login_ready_ = true;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginAccepted();
      }
      NotifyLoginReady();
      return;
    }
    login_ready_ = false;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordLoginRejected();
    }
  }

  void AdvanceApplicationHeartbeat(std::uint64_t now_ns) noexcept {
    if (!active_ || application_last_ping_ns_ == 0) {
      return;
    }
    if (application_awaiting_pong_) {
      if (now_ns - application_last_ping_ns_ >=
          application_heartbeat_timeout_ns_) {
        login_ready_ = false;
        active_ = false;
        application_awaiting_pong_ = false;
        if constexpr (DiagnosticsEnabled) {
          diagnostics_.RecordHeartbeatTimeout();
        }
        NotifyLoginNotReady();
        client_.RequestReconnect(
            websocket::ConnectionError::kHeartbeatTimeout,
            websocket::ReconnectTrigger::kHeartbeatTimeout);
      }
      return;
    }
    if (now_ns - application_last_ping_ns_ <
        application_heartbeat_interval_ns_) {
      return;
    }
    if (MapSendStatus(SendText("ping")) != OrderSendStatus::kOk) {
      return;
    }
    application_last_ping_ns_ = now_ns;
    application_awaiting_pong_ = true;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordPingSent();
    }
  }

  void RecordCorrelationMismatch() noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordCorrelationMismatch();
    }
  }

  void LogOrderSend(OrderRequestType request_type, std::uint64_t local_order_id,
                    std::uint64_t request_sequence,
                    std::int64_t request_send_local_ns) const noexcept {
    if (::nova::kLogManager.logger() == nullptr) {
      return;
    }
    NOVA_INFO(
        "bitget_order_send request_type={} request_sequence={} "
        "local_order_id={} request_send_local_ns={} inflight={}",
        magic_enum::enum_name(request_type), request_sequence, local_order_id,
        request_send_local_ns, inflight_count());
  }

  static void LogOrderResponse(const OrderResponse& response) noexcept {
    if (::nova::kLogManager.logger() == nullptr) {
      return;
    }
    NOVA_INFO(
        "bitget_order_response response_kind={} request_type={} "
        "request_sequence={} local_order_id={} exchange_order_id={} "
        "error_code={} request_send_local_ns={} local_receive_ns={} "
        "exchange_ns={} ack_rtt_ns={} connection_id_hash={}",
        magic_enum::enum_name(response.kind),
        magic_enum::enum_name(response.request_type), response.request_sequence,
        response.local_order_id, response.exchange_order_id,
        response.error_code, response.request_send_local_ns,
        response.local_receive_ns, response.exchange_ns, response.ack_rtt_ns,
        response.connection_id_hash);
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

  [[nodiscard]] static OrderResponseKind MapResponseKind(
      OperationResponseKind kind) noexcept {
    switch (kind) {
      case OperationResponseKind::kAck:
        return OrderResponseKind::kAck;
      case OperationResponseKind::kRejected:
        return OrderResponseKind::kRejected;
      case OperationResponseKind::kCancelRejected:
        return OrderResponseKind::kCancelRejected;
      case OperationResponseKind::kUnknownResult:
        return OrderResponseKind::kUnknownResult;
      case OperationResponseKind::kUnknown:
      case OperationResponseKind::kLoginAccepted:
      case OperationResponseKind::kLoginRejected:
        return OrderResponseKind::kUnknownResult;
    }
    return OrderResponseKind::kUnknownResult;
  }

  template <typename OrderT>
  [[nodiscard]] static OrderType OrderTypeFor(const OrderT& order) noexcept {
    if constexpr (requires { order.type; }) {
      return order.type;
    } else {
      return order.order_type;
    }
  }

  template <typename OrderT>
  [[nodiscard]] static std::uint64_t ParentIdFor(const OrderT& order) noexcept {
    if constexpr (requires { order.parent_id; }) {
      return order.parent_id;
    }
    return 0;
  }

  template <typename OrderT>
  [[nodiscard]] static std::uint16_t RouteIdFor(const OrderT& order) noexcept {
    if constexpr (requires { order.gateway_route_id; }) {
      return order.gateway_route_id;
    }
    return static_cast<std::uint16_t>(0xFFFF);
  }

  template <typename OrderT>
  [[nodiscard]] static detail::OrderRequestCorrelation MakeCorrelation(
      OrderRequestType type, const OrderT& order,
      std::uint64_t expected_exchange_order_id,
      std::int64_t request_send_local_ns) noexcept {
    return {.type = type,
            .local_order_id = order.local_order_id,
            .parent_id = ParentIdFor(order),
            .expected_exchange_order_id = expected_exchange_order_id,
            .request_send_local_ns = request_send_local_ns,
            .route_id = RouteIdFor(order)};
  }

  template <typename OrderT>
  [[nodiscard]] std::uint64_t ExchangeOrderIdForCancel(
      const OrderT& order) const noexcept {
    const std::uint64_t cached =
        exchange_order_id_for_local_order(order.local_order_id);
    if (cached != 0) {
      return cached;
    }
    if constexpr (requires { order.exchange_order_id; }) {
      return order.exchange_order_id;
    }
    return 0;
  }

  websocket::ConnectionConfig connection_;
  LoginCredentials credentials_;
  ResponseHandler& response_handler_;
  MessageHandler message_handler_;
  Client client_;
  simdjson::ondemand::parser text_parser_;
  absl::flat_hash_map<std::uint64_t, detail::OrderRequestCorrelation>
      request_correlations_;
  absl::flat_hash_map<std::uint64_t, std::uint64_t>
      local_order_id_to_exchange_order_id_;
  std::size_t request_map_capacity_;
  std::size_t order_id_cache_capacity_;
  std::size_t place_cache_reservations_{0};
  std::uint64_t request_sequence_{1};
  std::uint64_t application_heartbeat_interval_ns_{0};
  std::uint64_t application_heartbeat_timeout_ns_{0};
  std::uint64_t application_last_ping_ns_{0};
  bool active_{false};
  bool login_ready_{false};
  bool login_sent_{false};
  bool application_awaiting_pong_{false};
  Diagnostics diagnostics_{};
};

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SESSION_H_
