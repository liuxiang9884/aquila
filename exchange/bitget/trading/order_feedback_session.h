#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_FEEDBACK_SESSION_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_FEEDBACK_SESSION_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "core/trading/order_feedback_event.h"
#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"
#include "exchange/bitget/trading/operation_response_parser.h"
#include "exchange/bitget/trading/order_feedback_parser.h"
#include "exchange/bitget/trading/order_request_encoder.h"
#include "exchange/bitget/trading/order_types.h"
#include "exchange/common/simdjson_utils.h"
#include "nova/utils/log.h"
#include <simdjson.h>

namespace aquila::bitget {

struct OrderFeedbackSessionStats {
  std::uint64_t text_messages{0};
  std::uint64_t parse_errors{0};
  std::uint64_t ignored_messages{0};
  std::uint64_t login_sent{0};
  std::uint64_t login_accepted{0};
  std::uint64_t login_rejected{0};
  std::uint64_t subscribe_sent{0};
  std::uint64_t subscribe_send_failures{0};
  std::uint64_t subscribe_acks{0};
  std::uint64_t subscribe_errors{0};
  std::uint64_t pings_sent{0};
  std::uint64_t pongs_received{0};
  std::uint64_t heartbeat_timeouts{0};
  std::uint64_t events_published{0};
  std::uint64_t publish_failures{0};
  std::uint64_t decode_continuity_lost_events{0};
  std::uint64_t disconnect_continuity_lost_events{0};
  std::uint64_t global_continuity_lost_publish_failures{0};
  std::uint64_t connection_phase_transitions{0};
};

class NoopOrderFeedbackSessionDiagnostics {
 public:
  static constexpr bool kEnabled = false;

  [[nodiscard]] const OrderFeedbackSessionStats& stats() const noexcept {
    return kStats;
  }

 private:
  inline static constexpr OrderFeedbackSessionStats kStats{};
};

class OrderFeedbackSessionDiagnostics {
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
  void RecordSubscribeSent() noexcept {
    ++stats_.subscribe_sent;
  }
  void RecordSubscribeSendFailure() noexcept {
    ++stats_.subscribe_send_failures;
  }
  void RecordSubscribeAck() noexcept {
    ++stats_.subscribe_acks;
  }
  void RecordSubscribeError() noexcept {
    ++stats_.subscribe_errors;
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
  void RecordEventPublished() noexcept {
    ++stats_.events_published;
  }
  void RecordPublishFailure() noexcept {
    ++stats_.publish_failures;
  }
  void RecordDecodeContinuityLost() noexcept {
    ++stats_.decode_continuity_lost_events;
  }
  void RecordDisconnectContinuityLost() noexcept {
    ++stats_.disconnect_continuity_lost_events;
  }
  void RecordGlobalContinuityLostPublishFailure() noexcept {
    ++stats_.global_continuity_lost_publish_failures;
  }
  void RecordConnectionPhase() noexcept {
    ++stats_.connection_phase_transitions;
  }

  [[nodiscard]] const OrderFeedbackSessionStats& stats() const noexcept {
    return stats_;
  }

 private:
  OrderFeedbackSessionStats stats_{};
};

struct OrderFeedbackSessionDefaultTlsWebSocketPolicy
    : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::TlsSocket;
};

struct OrderFeedbackSessionDefaultPlainWebSocketPolicy
    : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::PlainSocket;
};

namespace detail {

inline constexpr std::string_view kOrderFeedbackSubscribeRequest =
    R"({"op":"subscribe","args":[{"instType":"UTA","topic":"order"}]})";

enum class OrderFeedbackControlEvent : std::uint8_t {
  kUnknown,
  kLogin,
  kSubscribe,
  kError,
};

struct OrderFeedbackControlEnvelope {
  OrderFeedbackControlEvent event{OrderFeedbackControlEvent::kUnknown};
  bool arg_is_order_topic{false};
  bool has_code{false};
  std::uint32_t code{0};
};

[[nodiscard]] inline OrderFeedbackControlEvent ParseControlEvent(
    std::string_view event) noexcept {
  if (event == "login") {
    return OrderFeedbackControlEvent::kLogin;
  }
  if (event == "subscribe") {
    return OrderFeedbackControlEvent::kSubscribe;
  }
  if (event == "error") {
    return OrderFeedbackControlEvent::kError;
  }
  return OrderFeedbackControlEvent::kUnknown;
}

[[nodiscard]] inline bool ParseControlDocument(
    simdjson::ondemand::document document,
    OrderFeedbackControlEnvelope* output) noexcept {
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    return false;
  }
  simdjson::ondemand::value value;
  std::string_view event_text;
  if (!FindSimdjsonField(root, "event", &value) ||
      !ReadSimdjsonString(value, &event_text)) {
    return false;
  }

  OrderFeedbackControlEnvelope envelope{};
  envelope.event = ParseControlEvent(event_text);
  simdjson::ondemand::object arg;
  if (FindSimdjsonObject(root, "arg", &arg)) {
    std::string_view inst_type;
    std::string_view topic;
    envelope.arg_is_order_topic =
        ReadStringField(arg, "instType", &inst_type) && inst_type == "UTA" &&
        ReadStringField(arg, "topic", &topic) && topic == "order";
  }
  if (FindSimdjsonField(root, "code", &value)) {
    std::uint64_t code = 0;
    if (!ReadOrderFeedbackUint64(value, &code) ||
        code > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    envelope.has_code = true;
    envelope.code = static_cast<std::uint32_t>(code);
  }
  *output = envelope;
  return true;
}

[[nodiscard]] inline bool ParseControlEnvelope(
    std::string_view payload, std::uint32_t readable_tail_bytes,
    simdjson::ondemand::parser& parser,
    OrderFeedbackControlEnvelope* output) noexcept {
  if (payload.empty()) {
    return false;
  }
  simdjson::ondemand::document document;
  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    const simdjson::padded_string_view view(
        payload.data(), payload.size(), payload.size() + readable_tail_bytes);
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      return false;
    }
    return ParseControlDocument(std::move(document), output);
  }
  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return false;
  }
  return ParseControlDocument(std::move(document), output);
}

}  // namespace detail

template <typename Publisher,
          typename WebSocketPolicy =
              OrderFeedbackSessionDefaultTlsWebSocketPolicy,
          typename Diagnostics = NoopOrderFeedbackSessionDiagnostics>
class OrderFeedbackSession {
 public:
  using TransportSocket = typename WebSocketPolicy::TransportSocket;
  using MessageHandler = websocket::MessageHandlerRef<OrderFeedbackSession>;
  using Client =
      websocket::BasicWebSocketClient<TransportSocket, MessageHandler>;
  static constexpr bool DiagnosticsEnabled = Diagnostics::kEnabled;
  static constexpr websocket::ClockSource kClockSource =
      WebSocketPolicy::kClockSource;

  OrderFeedbackSession(websocket::ConnectionConfig config,
                       LoginCredentials credentials, Publisher& publisher)
      : connection_(ApplyOptions(std::move(config))),
        credentials_(std::move(credentials)),
        publisher_(publisher),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(connection_, message_handler_),
        application_heartbeat_interval_ns_(
            static_cast<std::uint64_t>(connection_.heartbeat_interval_ms) *
            1'000'000ULL),
        application_heartbeat_timeout_ns_(
            static_cast<std::uint64_t>(connection_.heartbeat_timeout_ms) *
            1'000'000ULL) {
    client_.SetStateHook(this, &HandleState);
    client_.SetRuntimeLoopProbe(this, &HandleRuntimeLoopProbe);
    if constexpr (requires(const Publisher& publisher) {
                    publisher.HasPendingContinuityLostEvents();
                  }) {
      continuity_flush_pending_ = publisher_.HasPendingContinuityLostEvents();
    }
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
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordConnectionPhase();
    }
    if (phase == websocket::ConnectionPhase::kActive) {
      if (active_) {
        return;
      }
      active_ = true;
      ever_active_.store(true, std::memory_order_release);
      ++connection_generation_;
      ResetAuthenticatedState();
      decode_continuity_lost_published_ = false;
      application_awaiting_pong_ = false;
      application_last_ping_ns_ = websocket::NowNs(kClockSource);
      RetryPendingContinuityLostEvents(application_last_ping_ns_);
      LogPhase(phase, false);
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
      const bool active_before = active_;
      LogPhase(phase, active_before);
      active_ = false;
      ResetAuthenticatedState();
      application_awaiting_pong_ = false;
      application_last_ping_ns_ = 0;
      if (active_before) {
        PublishGlobalContinuityLost(
            OrderFeedbackContinuityReason::kSessionDisconnected,
            RealtimeNowNs());
        if constexpr (DiagnosticsEnabled) {
          diagnostics_.RecordDisconnectContinuityLost();
        }
      }
    }
  }

  [[nodiscard]] bool Ready() const noexcept {
    return active_ && login_ready_ && subscribed_;
  }
  [[nodiscard]] bool active() const noexcept {
    return active_;
  }
  [[nodiscard]] bool login_ready() const noexcept {
    return login_ready_;
  }
  [[nodiscard]] bool subscribed() const noexcept {
    return subscribed_;
  }
  [[nodiscard]] bool ever_active() const noexcept {
    return ever_active_.load(std::memory_order_acquire);
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
  [[nodiscard]] const websocket::ConnectionConfig& connection() const noexcept {
    return connection_;
  }
  [[nodiscard]] const OrderFeedbackSessionStats& stats() const noexcept {
    return diagnostics_.stats();
  }
  [[nodiscard]] const OrderFeedbackParserStats& parser_stats() const noexcept {
    return parser_stats_;
  }
  [[nodiscard]] std::string_view last_login_request() const noexcept {
    return last_login_request_;
  }
  [[nodiscard]] std::string_view last_subscribe_request() const noexcept {
    return last_subscribe_request_;
  }

#if defined(AQUILA_BITGET_ORDER_FEEDBACK_SESSION_ENABLE_TEST_HOOKS)
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
    static_cast<OrderFeedbackSession*>(context)->OnConnectionPhase(phase);
  }

  static void HandleRuntimeLoopProbe(
      void* context, websocket::RuntimeLoopProbePoint point) noexcept {
    if (point == websocket::RuntimeLoopProbePoint::kAfterRuntimeHook) {
      static_cast<OrderFeedbackSession*>(context)->AdvanceApplicationHeartbeat(
          websocket::NowNs(kClockSource));
    }
  }

  [[nodiscard]] static websocket::ConnectionConfig ApplyOptions(
      websocket::ConnectionConfig config) {
    config.runtime_policy.clock_source = kClockSource;
    return config;
  }

  [[nodiscard]] static std::int64_t RealtimeNowNs() noexcept {
    const std::uint64_t now = websocket::RealtimeClockNowNs();
    return now > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())
               ? std::numeric_limits<std::int64_t>::max()
               : static_cast<std::int64_t>(now);
  }

  void ResetAuthenticatedState() noexcept {
    login_ready_ = false;
    login_sent_ = false;
    subscribe_sent_ = false;
    subscribed_ = false;
  }

  websocket::DeliveryResult HandleText(
      const websocket::MessageView& view) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordTextMessage();
    }
    const std::string_view payload{
        reinterpret_cast<const char*>(view.payload.data()),
        view.payload.size()};
    const std::int64_t local_receive_ns = RealtimeNowNs();
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

    const OrderFeedbackParseResult parsed = ParseBitgetOrderFeedbackMessage(
        payload, view.readable_tail_bytes, local_receive_ns, feedback_parser_,
        parser_stats_, [this](const OrderFeedbackEvent& event) noexcept {
          return PublishEvent(event);
        });
    if (parsed.status == OrderFeedbackParseStatus::kControlMessage) {
      detail::OrderFeedbackControlEnvelope control;
      if (detail::ParseControlEnvelope(payload, view.readable_tail_bytes,
                                       control_parser_, &control)) {
        HandleControl(payload, view.readable_tail_bytes, control);
      } else if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseError();
      }
      return websocket::DeliveryResult::kAccepted;
    }
    if (parsed.status != OrderFeedbackParseStatus::kOk) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseError();
      }
      LogValidationError(parsed);
    }
    if (parsed.continuity_lost && !decode_continuity_lost_published_) {
      decode_continuity_lost_published_ = true;
      PublishGlobalContinuityLost(
          OrderFeedbackContinuityReason::kDecodeUnrecoverable,
          local_receive_ns);
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordDecodeContinuityLost();
      }
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void HandleControl(
      std::string_view payload, std::uint32_t readable_tail_bytes,
      const detail::OrderFeedbackControlEnvelope& control) noexcept {
    switch (control.event) {
      case detail::OrderFeedbackControlEvent::kLogin:
        HandleLoginOperation(payload, readable_tail_bytes);
        return;
      case detail::OrderFeedbackControlEvent::kSubscribe:
        HandleSubscribeAck(control);
        return;
      case detail::OrderFeedbackControlEvent::kError:
        if (control.arg_is_order_topic) {
          HandleSubscribeError(control);
        } else {
          HandleLoginOperation(payload, readable_tail_bytes);
        }
        return;
      case detail::OrderFeedbackControlEvent::kUnknown:
        if constexpr (DiagnosticsEnabled) {
          diagnostics_.RecordIgnoredMessage();
        }
        return;
    }
  }

  void HandleLoginOperation(std::string_view payload,
                            std::uint32_t readable_tail_bytes) noexcept {
    const OperationResponse response =
        ParseOperationResponse(payload, readable_tail_bytes, operation_parser_);
    if (response.parse_status != OperationParseStatus::kOk ||
        (response.kind != OperationResponseKind::kLoginAccepted &&
         response.kind != OperationResponseKind::kLoginRejected)) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseError();
      }
      return;
    }
    HandleLoginResponse(response);
  }

  void HandleLoginResponse(const OperationResponse& response) noexcept {
    if (response.kind == OperationResponseKind::kLoginRejected &&
        IsLoginReconnectError(response.error_code)) {
      ResetAuthenticatedState();
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginRejected();
      }
      LogLogin(false, response.error_code);
      RequestProtocolReconnect();
      return;
    }
    if (response.kind == OperationResponseKind::kLoginRejected &&
        login_ready_ &&
        IsAuthenticatedSessionInvalidatingError(response.error_code)) {
      ResetAuthenticatedState();
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginRejected();
      }
      LogLogin(false, response.error_code);
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
      LogLogin(true, 0);
      const OrderSendStatus subscribe_status = SendSubscribe();
      if (subscribe_status != OrderSendStatus::kOk) {
        ResetAuthenticatedState();
        if (IsTransientControlSendFailure(subscribe_status)) {
          RequestProtocolReconnect();
        }
      }
      return;
    }
    login_ready_ = false;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordLoginRejected();
    }
    LogLogin(false, response.error_code);
  }

  void HandleSubscribeAck(
      const detail::OrderFeedbackControlEnvelope& control) noexcept {
    if (!control.arg_is_order_topic || !active_ || !login_ready_ ||
        !subscribe_sent_) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordIgnoredMessage();
      }
      return;
    }
    if (control.has_code && control.code != 0) {
      HandleSubscribeError(control);
      return;
    }
    subscribe_sent_ = false;
    subscribed_ = true;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordSubscribeAck();
    }
    LogSubscribe(true, 0);
  }

  void HandleSubscribeError(
      const detail::OrderFeedbackControlEnvelope& control) noexcept {
    if (!IsAuthenticatedSessionInvalidatingError(control.code) &&
        (!control.arg_is_order_topic || !active_ || !login_ready_ ||
         !subscribe_sent_)) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordIgnoredMessage();
      }
      return;
    }
    subscribe_sent_ = false;
    subscribed_ = false;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordSubscribeError();
    }
    LogSubscribe(false, control.code);
    if (IsAuthenticatedSessionInvalidatingError(control.code)) {
      ResetAuthenticatedState();
      RequestProtocolReconnect();
    }
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
      return OrderSendStatus::kSignatureFailed;
    }
    last_login_request_.assign(encoded.text.data(), encoded.text.size());
    const OrderSendStatus status = SendText(last_login_request_);
    if (status == OrderSendStatus::kOk) {
      login_sent_ = true;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginSent();
      }
    }
    return status;
  }

  [[nodiscard]] OrderSendStatus SendSubscribe() noexcept {
    if (!login_ready_ || subscribe_sent_) {
      return OrderSendStatus::kWriteUnavailable;
    }
    last_subscribe_request_.assign(detail::kOrderFeedbackSubscribeRequest);
    const OrderSendStatus status = SendText(last_subscribe_request_);
    if (status == OrderSendStatus::kOk) {
      subscribe_sent_ = true;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordSubscribeSent();
      }
      return status;
    }
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordSubscribeSendFailure();
    }
    return status;
  }

  [[nodiscard]] OrderSendStatus SendText(std::string_view text) noexcept {
    const websocket::SendStatus status = client_.Core().SendText(
        std::as_bytes(std::span<const char>(text.data(), text.size())),
        websocket::WriteFlushMode::kTryFlushOne);
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

  void AdvanceApplicationHeartbeat(std::uint64_t now_ns) noexcept {
    RetryPendingContinuityLostEvents(now_ns);
    if (!active_ || application_last_ping_ns_ == 0) {
      return;
    }
    if (application_awaiting_pong_) {
      if (now_ns - application_last_ping_ns_ >=
          application_heartbeat_timeout_ns_) {
        ResetAuthenticatedState();
        application_awaiting_pong_ = false;
        if constexpr (DiagnosticsEnabled) {
          diagnostics_.RecordHeartbeatTimeout();
        }
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
    if (SendText("ping") != OrderSendStatus::kOk) {
      return;
    }
    application_last_ping_ns_ = now_ns;
    application_awaiting_pong_ = true;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordPingSent();
    }
  }

  bool PublishEvent(const OrderFeedbackEvent& event) noexcept {
    const bool ok = publisher_.Publish(event);
    if (ok) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordEventPublished();
      }
    } else {
      continuity_flush_pending_ = true;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordPublishFailure();
      }
    }
    LogRawUpdate(event, ok);
    return ok;
  }

  void PublishGlobalContinuityLost(OrderFeedbackContinuityReason reason,
                                   std::int64_t local_receive_ns) noexcept {
    const bool ok =
        publisher_.PublishGlobalContinuityLost(reason, local_receive_ns);
    if (!ok) {
      continuity_flush_pending_ = true;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordGlobalContinuityLostPublishFailure();
        diagnostics_.RecordPublishFailure();
      }
    }
    if (::nova::kLogManager.logger() != nullptr) {
      NOVA_WARNING(
          "bitget_order_feedback_continuity_lost reason={} publish_ok={} "
          "connection_generation={} local_receive_ns={}",
          magic_enum::enum_name(reason), ok ? "true" : "false",
          connection_generation_, local_receive_ns);
    }
  }

  [[nodiscard]] static bool IsTransientControlSendFailure(
      OrderSendStatus status) noexcept {
    return status == OrderSendStatus::kNoPreparedWriteSlot ||
           status == OrderSendStatus::kWriteUnavailable;
  }

  [[nodiscard]] static bool IsLoginReconnectError(std::uint32_t code) noexcept {
    return code == 30004 || code == 30007 || code == 30033;
  }

  [[nodiscard]] static bool IsAuthenticatedSessionInvalidatingError(
      std::uint32_t code) noexcept {
    return IsLoginReconnectError(code) || code == 30005 ||
           (code >= 30011 && code <= 30015);
  }

  void RequestProtocolReconnect() noexcept {
    client_.RequestReconnect(websocket::ConnectionError::kProtocolError,
                             websocket::ReconnectTrigger::kProtocolError);
  }

  void RetryPendingContinuityLostEvents(std::uint64_t now_ns) noexcept {
    if (!active_ || !continuity_flush_pending_ ||
        (continuity_flush_last_attempt_ns_ != 0 &&
         now_ns - continuity_flush_last_attempt_ns_ <
             kContinuityFlushRetryIntervalNs)) {
      return;
    }
    continuity_flush_last_attempt_ns_ = now_ns;
    if constexpr (requires(Publisher& publisher) {
                    publisher.FlushPendingContinuityLostEvents();
                    publisher.HasPendingContinuityLostEvents();
                  }) {
      (void)publisher_.FlushPendingContinuityLostEvents();
      continuity_flush_pending_ = publisher_.HasPendingContinuityLostEvents();
    } else {
      continuity_flush_pending_ = false;
    }
  }

  void LogPhase(websocket::ConnectionPhase phase,
                bool active_before) const noexcept {
    if (::nova::kLogManager.logger() != nullptr) {
      NOVA_INFO(
          "bitget_order_feedback_session_phase phase={} active_before={} "
          "ready_before={} connection_generation={} last_error={} "
          "reconnect_trigger={} reconnect_errno={}",
          magic_enum::enum_name(phase), active_before ? "true" : "false",
          Ready() ? "true" : "false", connection_generation_,
          magic_enum::enum_name(client_.last_error()),
          magic_enum::enum_name(client_.last_reconnect_trigger()),
          client_.last_reconnect_errno());
    }
  }

  static void LogLogin(bool accepted, std::uint32_t code) noexcept {
    if (::nova::kLogManager.logger() != nullptr) {
      NOVA_INFO("bitget_order_feedback_login accepted={} code={}",
                accepted ? "true" : "false", code);
    }
  }

  static void LogSubscribe(bool accepted, std::uint32_t code) noexcept {
    if (::nova::kLogManager.logger() != nullptr) {
      NOVA_INFO("bitget_order_feedback_subscribe accepted={} code={}",
                accepted ? "true" : "false", code);
    }
  }

  static void LogRawUpdate(const OrderFeedbackEvent& event, bool ok) noexcept {
    if (::nova::kLogManager.logger() != nullptr) {
      NOVA_INFO(
          "bitget_order_feedback_raw_update kind={} local_order_id={} "
          "exchange_order_id={} cumulative_filled_quantity={} "
          "left_quantity={} cancelled_quantity={} fill_price={} "
          "exchange_update_ns={} local_receive_ns={} publish_ok={}",
          magic_enum::enum_name(event.kind), event.local_order_id,
          event.exchange_order_id, event.cumulative_filled_quantity,
          event.left_quantity, event.cancelled_quantity, event.fill_price,
          event.exchange_update_ns, event.local_receive_ns,
          ok ? "true" : "false");
    }
  }

  void LogValidationError(
      const OrderFeedbackParseResult& result) const noexcept {
    if (::nova::kLogManager.logger() != nullptr) {
      NOVA_WARNING(
          "bitget_order_feedback_validation_error status={} orders_seen={} "
          "events_emitted={} continuity_lost={} connection_generation={}",
          magic_enum::enum_name(result.status), result.orders_seen,
          result.events_emitted, result.continuity_lost ? "true" : "false",
          connection_generation_);
    }
  }

  websocket::ConnectionConfig connection_;
  LoginCredentials credentials_;
  Publisher& publisher_;
  MessageHandler message_handler_;
  Client client_;
  simdjson::ondemand::parser control_parser_;
  simdjson::ondemand::parser operation_parser_;
  simdjson::ondemand::parser feedback_parser_;
  OrderFeedbackParserStats parser_stats_{};
  [[no_unique_address]] Diagnostics diagnostics_{};
  std::string last_login_request_;
  std::string last_subscribe_request_;
  std::atomic<bool> ever_active_{false};
  std::uint64_t connection_generation_{0};
  std::uint64_t application_heartbeat_interval_ns_{0};
  std::uint64_t application_heartbeat_timeout_ns_{0};
  std::uint64_t application_last_ping_ns_{0};
  std::uint64_t continuity_flush_last_attempt_ns_{0};
  bool active_{false};
  bool login_ready_{false};
  bool login_sent_{false};
  bool subscribe_sent_{false};
  bool subscribed_{false};
  bool application_awaiting_pong_{false};
  bool decode_continuity_lost_published_{false};
  bool continuity_flush_pending_{false};

  inline static constexpr std::uint64_t kContinuityFlushRetryIntervalNs =
      1'000'000;
};

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_FEEDBACK_SESSION_H_
