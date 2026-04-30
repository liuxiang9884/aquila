#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_SESSION_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_SESSION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/websocket/frame_codec.h"
#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/common/simdjson_utils.h"
#include "exchange/gate/market_data/client.h"
#include "exchange/gate/market_data/subscription.h"
#include <simdjson.h>

namespace aquila::gate {

enum class SubscriptionState : std::uint8_t {
  kIdle = 0,
  kSubscribeSent,
  kSubscribed,
  kUnsubscribeSent,
  kUnsubscribed,
  kRejected,
};

struct FuturesMarketDataSessionStats {
  std::uint64_t text_messages{0};
  std::uint64_t binary_messages{0};
  std::uint64_t non_final_messages{0};
  std::uint64_t control_messages{0};
  std::uint64_t control_parse_errors{0};
  std::uint64_t ignored_text_messages{0};
  std::uint64_t subscribe_sent{0};
  std::uint64_t subscribe_retry_attempts{0};
  std::uint64_t subscribe_send_failures{0};
  std::uint64_t subscribe_acks{0};
  std::uint64_t unsubscribe_sent{0};
  std::uint64_t unsubscribe_acks{0};
  std::uint64_t control_errors{0};
  std::uint64_t json_market_data_messages{0};
  std::uint64_t unsupported_json_market_data_messages{0};
};

namespace detail {

enum class TextEvent : std::uint8_t {
  kUnknown = 0,
  kSubscribe,
  kUnsubscribe,
  kUpdate,
};

struct TextEnvelope {
  TextEvent event{TextEvent::kUnknown};
  bool channel_is_book_ticker{false};
  bool result_success{false};
  bool has_error{false};
};

inline TextEvent ParseTextEvent(std::string_view event) noexcept {
  if (event == "subscribe") {
    return TextEvent::kSubscribe;
  }
  if (event == "unsubscribe") {
    return TextEvent::kUnsubscribe;
  }
  if (event == "update") {
    return TextEvent::kUpdate;
  }
  return TextEvent::kUnknown;
}

inline bool ParseTextEnvelopeDocument(simdjson::ondemand::document document,
                                      TextEnvelope& output) noexcept {
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    return false;
  }

  TextEnvelope envelope{};
  simdjson::ondemand::value value;
  if (FindSimdjsonField(root, "channel", &value)) {
    std::string_view channel{};
    envelope.channel_is_book_ticker =
        ReadSimdjsonString(value, &channel) && channel == "futures.book_ticker";
  }
  if (FindSimdjsonField(root, "event", &value)) {
    std::string_view event{};
    if (ReadSimdjsonString(value, &event)) {
      envelope.event = ParseTextEvent(event);
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

inline bool ParseTextEnvelope(std::string_view payload,
                              std::uint32_t readable_tail_bytes,
                              simdjson::ondemand::parser& parser,
                              TextEnvelope& output) noexcept {
  if (payload.empty()) {
    return false;
  }

  simdjson::ondemand::document document;
  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    simdjson::padded_string_view view(payload.data(), payload.size(),
                                      payload.size() + readable_tail_bytes);
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      return false;
    }
    return ParseTextEnvelopeDocument(std::move(document), output);
  }

  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return false;
  }
  return ParseTextEnvelopeDocument(std::move(document), output);
}

inline void BuildSymbolViews(std::span<const SymbolBinding> symbols,
                             std::vector<std::string_view>* output) {
  output->clear();
  output->reserve(symbols.size());
  for (const SymbolBinding& symbol : symbols) {
    output->push_back(symbol.symbol);
  }
}

}  // namespace detail

template <typename Consumer, typename TransportSocketT = websocket::TlsSocket,
          typename DiagnosticsT = NoopFuturesMarketDataDiagnostics>
class FuturesMarketDataSession {
 public:
  using MessageHandler = websocket::MessageHandlerRef<FuturesMarketDataSession>;
  using Client =
      websocket::BasicWebSocketClient<TransportSocketT, MessageHandler>;

  FuturesMarketDataSession(
      websocket::ConnectionConfig config,
      std::span<const SymbolBinding> symbols, Consumer& consumer,
      websocket::ClockSource clock_source = websocket::ClockSource::kSteady)
      : symbols_(symbols),
        market_data_client_(symbols_, consumer, clock_source),
        clock_source_(clock_source),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(std::move(config), message_handler_) {
    detail::BuildSymbolViews(symbols_, &subscription_symbols_);
    client_.SetStateHandler(this, &HandleState);
    client_.SetErrorHandler(this, &HandleError);
  }

  template <size_t N>
  FuturesMarketDataSession(
      websocket::ConnectionConfig config,
      const std::array<SymbolBinding, N>& symbols, Consumer& consumer,
      websocket::ClockSource clock_source = websocket::ClockSource::kSteady)
      : FuturesMarketDataSession(std::move(config),
                                 std::span<const SymbolBinding>(symbols),
                                 consumer, clock_source) {}

  bool Start() noexcept {
    return client_.Start();
  }

  void Stop() noexcept {
    client_.Stop();
  }

  void SetStateHandler(void* context,
                       websocket::StateHandler handler) noexcept {
    state_context_ = context;
    state_handler_ = handler;
  }

  void SetErrorHandler(void* context,
                       websocket::ErrorHandler handler) noexcept {
    error_context_ = context;
    error_handler_ = handler;
  }

  websocket::DeliveryResult Handle(
      const websocket::MessageView& view) noexcept {
    if (view.kind == websocket::PayloadKind::kBinary) [[likely]] {
      if (!view.fin) {
        ++stats_.non_final_messages;
        return websocket::DeliveryResult::kAccepted;
      }
      ++stats_.binary_messages;
      const std::int64_t local_ns =
          static_cast<std::int64_t>(websocket::NowNs(clock_source_));
      return market_data_client_.OnBinaryPayload(view.payload, local_ns);
    }

    if (!view.fin) {
      ++stats_.non_final_messages;
      return websocket::DeliveryResult::kAccepted;
    }

    if (view.kind == websocket::PayloadKind::kText) {
      return HandleText(view);
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void OnConnectionPhase(websocket::ConnectionPhase phase) noexcept {
    phase_ = phase;
    if (phase == websocket::ConnectionPhase::kActive) {
      if (!subscription_sent_for_connection_) {
        (void)SendSubscribeAttempt();
      }
      if (state_handler_ != nullptr) {
        state_handler_(state_context_, phase);
      }
      return;
    }

    if (phase == websocket::ConnectionPhase::kDisconnected ||
        phase == websocket::ConnectionPhase::kReconnectBackoff ||
        phase == websocket::ConnectionPhase::kClosing ||
        phase == websocket::ConnectionPhase::kClosed) {
      subscription_sent_for_connection_ = false;
      if (subscription_state_ == SubscriptionState::kSubscribeSent ||
          subscription_state_ == SubscriptionState::kSubscribed) {
        subscription_state_ = SubscriptionState::kIdle;
      }
    }

    if (state_handler_ != nullptr) {
      state_handler_(state_context_, phase);
    }
  }

  void OnConnectionError(websocket::ConnectionError error) noexcept {
    last_error_ = error;
    if (error_handler_ != nullptr) {
      error_handler_(error_context_, error);
    }
  }

  websocket::SendStatus RequestUnsubscribe() noexcept {
    const websocket::SendStatus status = SendUnsubscribe();
    unsubscribe_status_ = status;
    return status;
  }

  websocket::SendStatus RetryPendingSubscribe() noexcept {
    if (phase_ != websocket::ConnectionPhase::kActive ||
        subscription_sent_for_connection_ ||
        subscription_state_ == SubscriptionState::kRejected) {
      return subscribe_status_;
    }

    ++stats_.subscribe_retry_attempts;
    return SendSubscribeAttempt();
  }

  [[nodiscard]] SubscriptionState subscription_state() const noexcept {
    return subscription_state_;
  }

  [[nodiscard]] websocket::SendStatus subscribe_status() const noexcept {
    return subscribe_status_;
  }

  [[nodiscard]] websocket::SendStatus unsubscribe_status() const noexcept {
    return unsubscribe_status_;
  }

  [[nodiscard]] websocket::ConnectionPhase phase() const noexcept {
    return phase_;
  }

  [[nodiscard]] websocket::ConnectionError last_error() const noexcept {
    return last_error_;
  }

  [[nodiscard]] const FuturesMarketDataSessionStats& stats() const noexcept {
    return stats_;
  }

  [[nodiscard]] const DiagnosticsT& market_data_client_diagnostics()
      const noexcept {
    return market_data_client_.diagnostics();
  }

  [[nodiscard]] websocket::Metrics SnapshotMetrics() const noexcept {
    return client_.SnapshotMetrics();
  }

  [[nodiscard]] int NativeFd() noexcept {
    return client_.Core().NativeFd();
  }

  [[nodiscard]] std::string_view last_subscribe_request() const noexcept {
    return last_subscribe_request_;
  }

 private:
  static void HandleState(void* context,
                          websocket::ConnectionPhase phase) noexcept {
    static_cast<FuturesMarketDataSession*>(context)->OnConnectionPhase(phase);
  }

  static void HandleError(void* context,
                          websocket::ConnectionError error) noexcept {
    static_cast<FuturesMarketDataSession*>(context)->OnConnectionError(error);
  }

  websocket::DeliveryResult HandleText(
      const websocket::MessageView& view) noexcept {
    ++stats_.text_messages;
    const std::string_view payload{
        reinterpret_cast<const char*>(view.payload.data()),
        view.payload.size()};

    detail::TextEnvelope envelope{};
    if (!detail::ParseTextEnvelope(payload, view.readable_tail_bytes,
                                   text_parser_, envelope)) {
      ++stats_.control_parse_errors;
      return websocket::DeliveryResult::kAccepted;
    }

    switch (envelope.event) {
      case detail::TextEvent::kSubscribe:
        HandleSubscribeResponse(envelope);
        return websocket::DeliveryResult::kAccepted;
      case detail::TextEvent::kUnsubscribe:
        HandleUnsubscribeResponse(envelope);
        return websocket::DeliveryResult::kAccepted;
      case detail::TextEvent::kUpdate:
        ++stats_.json_market_data_messages;
        ++stats_.unsupported_json_market_data_messages;
        return websocket::DeliveryResult::kAccepted;
      case detail::TextEvent::kUnknown:
        ++stats_.ignored_text_messages;
        return websocket::DeliveryResult::kAccepted;
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void HandleSubscribeResponse(const detail::TextEnvelope& envelope) noexcept {
    ++stats_.control_messages;
    if (!envelope.channel_is_book_ticker) {
      ++stats_.ignored_text_messages;
      return;
    }
    if (envelope.has_error || !envelope.result_success) {
      ++stats_.control_errors;
      subscription_state_ = SubscriptionState::kRejected;
      return;
    }
    ++stats_.subscribe_acks;
    subscription_state_ = SubscriptionState::kSubscribed;
  }

  void HandleUnsubscribeResponse(
      const detail::TextEnvelope& envelope) noexcept {
    ++stats_.control_messages;
    if (!envelope.channel_is_book_ticker) {
      ++stats_.ignored_text_messages;
      return;
    }
    if (envelope.has_error || !envelope.result_success) {
      ++stats_.control_errors;
      subscription_state_ = SubscriptionState::kRejected;
      return;
    }
    ++stats_.unsubscribe_acks;
    subscription_state_ = SubscriptionState::kUnsubscribed;
  }

  websocket::SendStatus SendSubscribe() noexcept {
    last_subscribe_request_ = BuildFuturesBookTickerSubscribeRequest(
        std::span<const std::string_view>(subscription_symbols_.data(),
                                          subscription_symbols_.size()),
        static_cast<std::int64_t>(std::time(nullptr)));
    const websocket::SendStatus status = SendText(last_subscribe_request_);
    if (status == websocket::SendStatus::kOk) {
      ++stats_.subscribe_sent;
      subscription_state_ = SubscriptionState::kSubscribeSent;
    }
    return status;
  }

  websocket::SendStatus SendSubscribeAttempt() noexcept {
    const websocket::SendStatus status = SendSubscribe();
    subscribe_status_ = status;
    subscription_sent_for_connection_ = status == websocket::SendStatus::kOk;
    if (status != websocket::SendStatus::kOk) {
      ++stats_.subscribe_send_failures;
    }
    return status;
  }

  websocket::SendStatus SendUnsubscribe() noexcept {
    const std::string request = BuildFuturesBookTickerUnsubscribeRequest(
        std::span<const std::string_view>(subscription_symbols_.data(),
                                          subscription_symbols_.size()),
        static_cast<std::int64_t>(std::time(nullptr)));
    const websocket::SendStatus status = SendText(request);
    if (status == websocket::SendStatus::kOk) {
      ++stats_.unsubscribe_sent;
      subscription_state_ = SubscriptionState::kUnsubscribeSent;
    }
    return status;
  }

  websocket::SendStatus SendText(std::string_view payload_text) noexcept {
    auto& core = client_.Core();
    websocket::PreparedWrite* write = core.TryAcquirePreparedWrite();
    if (write == nullptr) {
      return websocket::SendStatus::kNoPreparedWriteSlot;
    }

    const auto payload = std::as_bytes(
        std::span<const char>(payload_text.data(), payload_text.size()));
    const auto encoded = encoder_.EncodeText(payload, write->storage);
    if (!encoded.ok) {
      core.CancelPreparedWrite(write);
      return websocket::SendStatus::kEncodeFailed;
    }

    write->encoded_size = static_cast<std::uint32_t>(encoded.bytes.size());
    write->write_offset = 0;
    write->kind = websocket::PayloadKind::kText;
    const websocket::SendStatus status = core.CommitPreparedWrite(write);
    if (status != websocket::SendStatus::kOk) {
      core.CancelPreparedWrite(write);
    }
    return status;
  }

  std::span<const SymbolBinding> symbols_;
  std::vector<std::string_view> subscription_symbols_;
  std::string last_subscribe_request_;
  FuturesMarketDataClient<Consumer, DiagnosticsT> market_data_client_;
  websocket::ClockSource clock_source_;
  websocket::FrameCodec encoder_{4096, 4096};
  MessageHandler message_handler_;
  Client client_;
  FuturesMarketDataSessionStats stats_{};
  simdjson::ondemand::parser text_parser_;
  SubscriptionState subscription_state_{SubscriptionState::kIdle};
  websocket::SendStatus subscribe_status_{
      websocket::SendStatus::kWriteUnavailable};
  websocket::SendStatus unsubscribe_status_{
      websocket::SendStatus::kWriteUnavailable};
  websocket::ConnectionPhase phase_{websocket::ConnectionPhase::kDisconnected};
  websocket::ConnectionError last_error_{websocket::ConnectionError::kNone};
  void* state_context_{nullptr};
  websocket::StateHandler state_handler_{nullptr};
  void* error_context_{nullptr};
  websocket::ErrorHandler error_handler_{nullptr};
  bool subscription_sent_for_connection_{false};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_SESSION_H_
