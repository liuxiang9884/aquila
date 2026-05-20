#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_FEEDBACK_SESSION_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_FEEDBACK_SESSION_H_

#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/common/simdjson_utils.h"
#include "exchange/gate/trading/order_feedback_parser.h"
#include "exchange/gate/trading/order_request_encoder.h"
#include "exchange/gate/trading/order_session.h"
#include "exchange/gate/trading/submit_response_parser.h"
#include <simdjson.h>

namespace aquila::gate {

struct OrderFeedbackSessionStats {
  std::uint64_t text_messages{0};
  std::uint64_t binary_messages{0};
  std::uint64_t control_messages{0};
  std::uint64_t control_parse_errors{0};
  std::uint64_t ignored_text_messages{0};

  std::uint64_t login_sent{0};
  std::uint64_t login_accepted{0};
  std::uint64_t login_rejected{0};
  std::uint64_t subscribe_sent{0};
  std::uint64_t subscribe_send_failures{0};
  std::uint64_t subscribe_acks{0};
  std::uint64_t control_errors{0};

  std::uint64_t parse_errors{0};
  std::uint64_t events_published{0};
  std::uint64_t publish_failures{0};
  std::uint64_t global_continuity_lost_events_published{0};
  std::uint64_t global_continuity_lost_publish_failures{0};
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
  void RecordBinaryMessage() noexcept {
    ++stats_.binary_messages;
  }
  void RecordControlMessage() noexcept {
    ++stats_.control_messages;
  }
  void RecordControlParseError() noexcept {
    ++stats_.control_parse_errors;
  }
  void RecordIgnoredTextMessage() noexcept {
    ++stats_.ignored_text_messages;
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
  void RecordControlError() noexcept {
    ++stats_.control_errors;
  }
  void RecordParseError() noexcept {
    ++stats_.parse_errors;
  }
  void RecordEventPublished() noexcept {
    ++stats_.events_published;
  }
  void RecordPublishFailure() noexcept {
    ++stats_.publish_failures;
  }
  void RecordGlobalContinuityLostPublished() noexcept {
    ++stats_.global_continuity_lost_events_published;
  }
  void RecordGlobalContinuityLostPublishFailure() noexcept {
    ++stats_.global_continuity_lost_publish_failures;
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

enum class OrderFeedbackControlEvent : std::uint8_t {
  kUnknown = 0,
  kSubscribe,
  kUpdate,
};

struct OrderFeedbackControlEnvelope {
  OrderFeedbackControlEvent event{OrderFeedbackControlEvent::kUnknown};
  bool channel_is_orders{false};
  bool result_success{false};
  bool has_error{false};
};

inline OrderFeedbackControlEvent ParseOrderFeedbackControlEvent(
    std::string_view event) noexcept {
  if (event == "subscribe") {
    return OrderFeedbackControlEvent::kSubscribe;
  }
  if (event == "update") {
    return OrderFeedbackControlEvent::kUpdate;
  }
  return OrderFeedbackControlEvent::kUnknown;
}

inline bool ParseOrderFeedbackControlEnvelopeDocument(
    simdjson::ondemand::document document,
    OrderFeedbackControlEnvelope& output) noexcept {
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    return false;
  }

  OrderFeedbackControlEnvelope envelope{};
  simdjson::ondemand::value value;
  if (FindSimdjsonField(root, "channel", &value)) {
    std::string_view channel{};
    envelope.channel_is_orders =
        ReadSimdjsonString(value, &channel) && channel == "futures.orders";
  }
  if (FindSimdjsonField(root, "event", &value)) {
    std::string_view event{};
    if (ReadSimdjsonString(value, &event)) {
      envelope.event = ParseOrderFeedbackControlEvent(event);
    }
  }
  envelope.has_error = FindSimdjsonField(root, "error", &value);

  simdjson::ondemand::object result;
  if (FindSimdjsonObject(root, "result", &result) &&
      FindSimdjsonField(result, "status", &value)) {
    std::string_view status{};
    envelope.result_success =
        ReadSimdjsonString(value, &status) && status == "success";
  }

  output = envelope;
  return true;
}

inline bool ParseOrderFeedbackControlEnvelope(
    std::string_view payload, std::uint32_t readable_tail_bytes,
    simdjson::ondemand::parser& parser,
    OrderFeedbackControlEnvelope& output) noexcept {
  if (payload.empty()) {
    return false;
  }

  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    simdjson::padded_string_view view(payload.data(), payload.size(),
                                      payload.size() + readable_tail_bytes);
    simdjson::ondemand::document document;
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      return false;
    }
    return ParseOrderFeedbackControlEnvelopeDocument(std::move(document),
                                                     output);
  }

  simdjson::padded_string padded(payload);
  simdjson::ondemand::document document;
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return false;
  }
  return ParseOrderFeedbackControlEnvelopeDocument(std::move(document), output);
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
        client_(connection_, message_handler_) {
    client_.SetStateHook(this, &HandleState);
  }

  bool Start() noexcept {
    return client_.Start();
  }

  bool Run() noexcept {
    ScopedStopHandlers stop_handlers(this);
    return Start();
  }

  void Stop() noexcept {
    client_.Stop();
  }

  websocket::DeliveryResult Handle(
      const websocket::MessageView& view) noexcept {
    if (view.kind == websocket::PayloadKind::kBinary) [[likely]] {
      return HandleBinary(view);
    }
    if (view.kind == websocket::PayloadKind::kText) {
      return HandleText(view);
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void OnConnectionPhase(websocket::ConnectionPhase phase) noexcept {
    if (phase == websocket::ConnectionPhase::kActive) {
      active_ = true;
      ever_active_.store(true, std::memory_order_release);
      ResetAuthenticatedState();
      (void)SendLogin();
      return;
    }

    if (phase == websocket::ConnectionPhase::kDisconnected ||
        phase == websocket::ConnectionPhase::kReconnectBackoff ||
        phase == websocket::ConnectionPhase::kClosing ||
        phase == websocket::ConnectionPhase::kClosed) {
      const bool publish_continuity_lost = active_;
      active_ = false;
      ResetAuthenticatedState();
      if (publish_continuity_lost) {
        PublishGlobalContinuityLost(
            OrderFeedbackContinuityReason::kSessionDisconnected, NowNsInt64());
      }
    }
  }

  [[nodiscard]] bool login_ready() const noexcept {
    return login_ready_;
  }

  [[nodiscard]] bool ready() const noexcept {
    return login_ready_ && subscribed_;
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

  [[nodiscard]] const websocket::ConnectionConfig& connection() const noexcept {
    return connection_;
  }

  [[nodiscard]] const OrderFeedbackSessionStats& stats() const noexcept {
    return diagnostics_.stats();
  }

  [[nodiscard]] const OrderFeedbackParserStats& parser_stats() const noexcept {
    return parser_stats_;
  }

  [[nodiscard]] websocket::Metrics SnapshotMetrics() const noexcept {
    return client_.SnapshotMetrics();
  }

  [[nodiscard]] int NativeFd() noexcept {
    return client_.Core().NativeFd();
  }

  [[nodiscard]] std::string_view last_login_request() const noexcept {
    return last_login_request_;
  }

  [[nodiscard]] std::string_view last_subscribe_request() const noexcept {
    return last_subscribe_request_;
  }

 private:
  class ScopedStopHandlers {
   public:
    explicit ScopedStopHandlers(OrderFeedbackSession* session) noexcept
        : session_(session) {
      active_stop_session_.store(session_, std::memory_order_release);
      previous_int_handler_ = std::signal(SIGINT, &HandleStopSignal);
      previous_term_handler_ = std::signal(SIGTERM, &HandleStopSignal);
    }

    ScopedStopHandlers(const ScopedStopHandlers&) = delete;
    ScopedStopHandlers& operator=(const ScopedStopHandlers&) = delete;

    ~ScopedStopHandlers() {
      std::signal(SIGINT, previous_int_handler_);
      std::signal(SIGTERM, previous_term_handler_);
      OrderFeedbackSession* expected = session_;
      (void)active_stop_session_.compare_exchange_strong(
          expected, nullptr, std::memory_order_acq_rel,
          std::memory_order_acquire);
    }

   private:
    using SignalHandler = void (*)(int);

    OrderFeedbackSession* session_{nullptr};
    SignalHandler previous_int_handler_{SIG_DFL};
    SignalHandler previous_term_handler_{SIG_DFL};
  };

  static void HandleStopSignal(int signal) noexcept {
    if (signal != SIGINT && signal != SIGTERM) {
      return;
    }
    OrderFeedbackSession* session =
        active_stop_session_.load(std::memory_order_acquire);
    if (session != nullptr) {
      session->Stop();
    }
  }

  static void HandleState(void* context,
                          websocket::ConnectionPhase phase) noexcept {
    static_cast<OrderFeedbackSession*>(context)->OnConnectionPhase(phase);
  }

  static websocket::ConnectionConfig ApplyOptions(
      websocket::ConnectionConfig config) {
    config.runtime_policy.clock_source = kClockSource;
    return config;
  }

  [[nodiscard]] static std::int64_t NowNsInt64() noexcept {
    const std::uint64_t now = websocket::NowNs(kClockSource);
    if (now >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(now);
  }

  [[nodiscard]] static std::int64_t NowSeconds() noexcept {
    return static_cast<std::int64_t>(std::time(nullptr));
  }

  [[nodiscard]] std::uint64_t NextRequestSequence() noexcept {
    return request_sequence_++;
  }

  void ResetAuthenticatedState() noexcept {
    login_ready_ = false;
    subscribed_ = false;
    subscribe_sent_ = false;
    login_uid_ = 0;
    login_request_sequence_ = 0;
  }

  [[nodiscard]] websocket::DeliveryResult HandleBinary(
      const websocket::MessageView& view) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordBinaryMessage();
    }

    const std::string_view payload{
        reinterpret_cast<const char*>(view.payload.data()),
        view.payload.size()};
    const OrderFeedbackParseResult parsed = ParseGateOrderFeedbackMessage(
        payload, NowNsInt64(), parser_stats_,
        [this](const OrderFeedbackEvent& event) noexcept {
          PublishEvent(event);
        });
    if (parsed.status != OrderFeedbackParseStatus::kOk) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseError();
      }
    }
    return websocket::DeliveryResult::kAccepted;
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
    if (parsed.parse_status == GateSubmitParseStatus::kOk &&
        parsed.request_id.ok &&
        parsed.request_id.type == OrderRequestType::kLogin) {
      HandleLoginResponse(parsed);
      return websocket::DeliveryResult::kAccepted;
    }

    detail::OrderFeedbackControlEnvelope envelope{};
    if (!detail::ParseOrderFeedbackControlEnvelope(
            payload, view.readable_tail_bytes, text_parser_, envelope)) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordControlParseError();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    switch (envelope.event) {
      case detail::OrderFeedbackControlEvent::kSubscribe:
        HandleSubscribeResponse(envelope);
        return websocket::DeliveryResult::kAccepted;
      case detail::OrderFeedbackControlEvent::kUpdate:
      case detail::OrderFeedbackControlEvent::kUnknown:
        if constexpr (DiagnosticsEnabled) {
          diagnostics_.RecordIgnoredTextMessage();
        }
        return websocket::DeliveryResult::kAccepted;
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void HandleLoginResponse(const GateSubmitResponse& parsed) noexcept {
    if (login_request_sequence_ == 0 ||
        parsed.request_id.sequence != login_request_sequence_) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordIgnoredTextMessage();
      }
      return;
    }
    login_request_sequence_ = 0;
    if (parsed.channel != kFuturesLogin ||
        parsed.kind == GateSubmitResponseKind::kUnknown) {
      login_ready_ = false;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordIgnoredTextMessage();
      }
      return;
    }

    if (parsed.http_status == 200 &&
        parsed.kind == GateSubmitResponseKind::kResult &&
        parsed.has_login_uid) {
      login_ready_ = true;
      login_uid_ = parsed.login_uid;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginAccepted();
      }
      (void)SendSubscribe();
      return;
    }

    login_ready_ = false;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordLoginRejected();
    }
  }

  void HandleSubscribeResponse(
      const detail::OrderFeedbackControlEnvelope& envelope) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordControlMessage();
    }
    if (!envelope.channel_is_orders) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordIgnoredTextMessage();
      }
      return;
    }
    if (!login_ready_ || !subscribe_sent_) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordIgnoredTextMessage();
      }
      return;
    }
    if (envelope.has_error || !envelope.result_success) {
      subscribed_ = false;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordControlError();
      }
      return;
    }

    subscribed_ = true;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordSubscribeAck();
    }
  }

  [[nodiscard]] websocket::SendStatus SendLogin() noexcept {
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
      return websocket::SendStatus::kEncodeFailed;
    }

    last_login_request_.assign(encoded.text.data(), encoded.text.size());
    const websocket::SendStatus status = SendText(last_login_request_);
    if (status == websocket::SendStatus::kOk) {
      login_request_sequence_ = sequence;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginSent();
      }
    }
    return status;
  }

  [[nodiscard]] websocket::SendStatus SendSubscribe() noexcept {
    if (login_uid_ == 0 || subscribe_sent_) {
      return websocket::SendStatus::kWriteUnavailable;
    }

    std::array<char, kOrderFeedbackSubscribeRequestBufferSize> buffer;
    const EncodedTextRequest encoded = EncodeOrderFeedbackSubscribeRequest(
        OrderFeedbackSubscribeRequestFields{
            .api_key = credentials_.api_key,
            .api_secret = credentials_.api_secret,
            .timestamp = NowSeconds(),
            .login_uid = login_uid_},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordSubscribeSendFailure();
      }
      return websocket::SendStatus::kEncodeFailed;
    }

    last_subscribe_request_.assign(encoded.text.data(), encoded.text.size());
    const websocket::SendStatus status = SendText(last_subscribe_request_);
    if (status == websocket::SendStatus::kOk) {
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

  websocket::SendStatus SendText(std::string_view payload_text) noexcept {
    auto& core = client_.Core();
    const auto payload = std::as_bytes(
        std::span<const char>(payload_text.data(), payload_text.size()));
    return core.SendText(payload, websocket::WriteFlushMode::kTryFlushOne);
  }

  void PublishEvent(const OrderFeedbackEvent& event) noexcept {
    if (publisher_.Publish(event)) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordEventPublished();
      }
      return;
    }
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordPublishFailure();
    }
  }

  void PublishGlobalContinuityLost(OrderFeedbackContinuityReason reason,
                                   std::int64_t local_receive_ns) noexcept {
    if (publisher_.PublishGlobalContinuityLost(reason, local_receive_ns)) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordGlobalContinuityLostPublished();
      }
      return;
    }
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordGlobalContinuityLostPublishFailure();
      diagnostics_.RecordPublishFailure();
    }
  }

  websocket::ConnectionConfig connection_;
  LoginCredentials credentials_;
  Publisher& publisher_;
  MessageHandler message_handler_;
  Client client_;
  [[no_unique_address]] Diagnostics diagnostics_{};
  simdjson::ondemand::parser text_parser_;
  OrderFeedbackParserStats parser_stats_{};
  std::string last_login_request_;
  std::string last_subscribe_request_;
  std::atomic<bool> ever_active_{false};
  std::uint64_t request_sequence_{1};
  std::uint64_t login_request_sequence_{0};
  std::uint64_t login_uid_{0};
  bool active_{false};
  bool login_ready_{false};
  bool subscribe_sent_{false};
  bool subscribed_{false};
  inline static std::atomic<OrderFeedbackSession*> active_stop_session_{
      nullptr};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_FEEDBACK_SESSION_H_
