#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_SESSION_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_SESSION_H_

#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/market_data/client.h"
#include "exchange/gate/market_data/subscription.h"
#include "exchange/gate/market_data/subscription_controller.h"
#include "exchange/gate/market_data/text_envelope_parser.h"
#include <simdjson.h>

namespace aquila::gate {

struct FuturesMarketDataSessionStats {
  std::uint64_t text_messages{0};
  std::uint64_t binary_messages{0};
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

struct NoopFuturesMarketDataSessionDiagnostics {
  static constexpr bool kEnabled = false;

  [[nodiscard]] const FuturesMarketDataSessionStats& stats() const noexcept {
    return kStats;
  }

 private:
  inline static constexpr FuturesMarketDataSessionStats kStats{};
};

class FuturesMarketDataSessionDiagnostics {
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

  void RecordSubscribeSent() noexcept {
    ++stats_.subscribe_sent;
  }

  void RecordSubscribeRetryAttempt() noexcept {
    ++stats_.subscribe_retry_attempts;
  }

  void RecordSubscribeSendFailure() noexcept {
    ++stats_.subscribe_send_failures;
  }

  void RecordSubscribeAck() noexcept {
    ++stats_.subscribe_acks;
  }

  void RecordUnsubscribeSent() noexcept {
    ++stats_.unsubscribe_sent;
  }

  void RecordUnsubscribeAck() noexcept {
    ++stats_.unsubscribe_acks;
  }

  void RecordControlError() noexcept {
    ++stats_.control_errors;
  }

  void RecordJsonMarketDataMessage() noexcept {
    ++stats_.json_market_data_messages;
  }

  void RecordUnsupportedJsonMarketDataMessage() noexcept {
    ++stats_.unsupported_json_market_data_messages;
  }

  [[nodiscard]] const FuturesMarketDataSessionStats& stats() const noexcept {
    return stats_;
  }

 private:
  FuturesMarketDataSessionStats stats_{};
};

namespace detail {

inline void BuildSymbolViews(std::span<const SymbolBinding> symbols,
                             std::vector<std::string_view>* output) {
  output->clear();
  output->reserve(symbols.size());
  for (const SymbolBinding& symbol : symbols) {
    output->push_back(symbol.exchange_symbol);
  }
}

}  // namespace detail

template <typename Consumer, typename TransportSocketT = websocket::TlsSocket,
          typename DiagnosticsT = NoopFuturesMarketDataDiagnostics,
          typename OptionsT = websocket::DefaultWebSocketOptions,
          typename SessionDiagnosticsT =
              NoopFuturesMarketDataSessionDiagnostics>
class FuturesMarketDataSession {
 public:
  using MessageHandler = websocket::MessageHandlerRef<FuturesMarketDataSession>;
  using Client =
      websocket::BasicWebSocketClient<TransportSocketT, MessageHandler>;
  static constexpr websocket::ClockSource kClockSource = OptionsT::kClockSource;
  static constexpr bool SessionDiagnosticsEnabled =
      SessionDiagnosticsT::kEnabled;

  FuturesMarketDataSession(websocket::ConnectionConfig config,
                           std::span<const SymbolBinding> symbols,
                           Consumer& consumer)
      : market_data_client_(symbols, consumer),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(ApplyOptions(std::move(config)), message_handler_) {
    detail::BuildSymbolViews(symbols, &subscription_symbols_);
    client_.SetStateHook(this, &HandleState);
  }

  template <size_t N>
  FuturesMarketDataSession(websocket::ConnectionConfig config,
                           const std::array<SymbolBinding, N>& symbols,
                           Consumer& consumer)
      : FuturesMarketDataSession(std::move(config),
                                 std::span<const SymbolBinding>(symbols),
                                 consumer) {}

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
      if constexpr (SessionDiagnosticsEnabled) {
        session_diagnostics_.RecordBinaryMessage();
      }
      const std::int64_t local_ns =
          static_cast<std::int64_t>(websocket::NowNs(kClockSource));
      return market_data_client_.OnBinaryPayload(view.payload, local_ns);
    }

    if (view.kind == websocket::PayloadKind::kText) {
      return HandleText(view);
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void OnConnectionPhase(websocket::ConnectionPhase phase) noexcept {
    if (phase == websocket::ConnectionPhase::kActive) {
      ever_active_.store(true, std::memory_order_release);
    }
    if (subscription_controller_.OnConnectionPhase(phase)) {
      (void)SendSubscribeAttempt();
    }
  }

  websocket::SendStatus RequestUnsubscribe() noexcept {
    return SendUnsubscribe();
  }

  websocket::SendStatus RetryPendingSubscribe() noexcept {
    if (!subscription_controller_.CanRetrySubscribe()) {
      return subscription_controller_.subscribe_status();
    }

    if constexpr (SessionDiagnosticsEnabled) {
      session_diagnostics_.RecordSubscribeRetryAttempt();
    }
    return SendSubscribeAttempt();
  }

  [[nodiscard]] SubscriptionState subscription_state() const noexcept {
    return subscription_controller_.subscription_state();
  }

  [[nodiscard]] websocket::SendStatus subscribe_status() const noexcept {
    return subscription_controller_.subscribe_status();
  }

  [[nodiscard]] websocket::SendStatus unsubscribe_status() const noexcept {
    return subscription_controller_.unsubscribe_status();
  }

  [[nodiscard]] websocket::ConnectionPhase phase() const noexcept {
    return client_.phase();
  }

  [[nodiscard]] websocket::ConnectionError last_error() const noexcept {
    return client_.last_error();
  }

  [[nodiscard]] bool ever_active() const noexcept {
    return ever_active_.load(std::memory_order_acquire);
  }

  [[nodiscard]] const FuturesMarketDataSessionStats& stats() const noexcept {
    return session_diagnostics_.stats();
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
  class ScopedStopHandlers {
   public:
    explicit ScopedStopHandlers(FuturesMarketDataSession* session) noexcept
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
      FuturesMarketDataSession* expected = session_;
      (void)active_stop_session_.compare_exchange_strong(
          expected, nullptr, std::memory_order_acq_rel,
          std::memory_order_acquire);
    }

   private:
    using SignalHandler = void (*)(int);

    FuturesMarketDataSession* session_{nullptr};
    SignalHandler previous_int_handler_{SIG_DFL};
    SignalHandler previous_term_handler_{SIG_DFL};
  };

  static void HandleStopSignal(int signal) noexcept {
    if (signal != SIGINT && signal != SIGTERM) {
      return;
    }
    FuturesMarketDataSession* session =
        active_stop_session_.load(std::memory_order_acquire);
    if (session != nullptr) {
      session->Stop();
    }
  }

  static void HandleState(void* context,
                          websocket::ConnectionPhase phase) noexcept {
    static_cast<FuturesMarketDataSession*>(context)->OnConnectionPhase(phase);
  }

  static websocket::ConnectionConfig ApplyOptions(
      websocket::ConnectionConfig config) {
    config.runtime_policy.clock_source = kClockSource;
    return config;
  }

  websocket::DeliveryResult HandleText(
      const websocket::MessageView& view) noexcept {
    if constexpr (SessionDiagnosticsEnabled) {
      session_diagnostics_.RecordTextMessage();
    }
    const std::string_view payload{
        reinterpret_cast<const char*>(view.payload.data()),
        view.payload.size()};

    detail::TextEnvelope envelope{};
    if (!detail::ParseTextEnvelope(payload, view.readable_tail_bytes,
                                   text_parser_, envelope)) {
      if constexpr (SessionDiagnosticsEnabled) {
        session_diagnostics_.RecordControlParseError();
      }
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
        if constexpr (SessionDiagnosticsEnabled) {
          session_diagnostics_.RecordJsonMarketDataMessage();
          session_diagnostics_.RecordUnsupportedJsonMarketDataMessage();
        }
        return websocket::DeliveryResult::kAccepted;
      case detail::TextEvent::kUnknown:
        if constexpr (SessionDiagnosticsEnabled) {
          session_diagnostics_.RecordIgnoredTextMessage();
        }
        return websocket::DeliveryResult::kAccepted;
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void HandleSubscribeResponse(const detail::TextEnvelope& envelope) noexcept {
    if constexpr (SessionDiagnosticsEnabled) {
      session_diagnostics_.RecordControlMessage();
    }
    if (!envelope.channel_is_book_ticker) {
      if constexpr (SessionDiagnosticsEnabled) {
        session_diagnostics_.RecordIgnoredTextMessage();
      }
      return;
    }
    if (envelope.has_error || !envelope.result_success) {
      if constexpr (SessionDiagnosticsEnabled) {
        session_diagnostics_.RecordControlError();
      }
      subscription_controller_.MarkSubscribeRejected();
      return;
    }
    if constexpr (SessionDiagnosticsEnabled) {
      session_diagnostics_.RecordSubscribeAck();
    }
    subscription_controller_.MarkSubscribeAccepted();
  }

  void HandleUnsubscribeResponse(
      const detail::TextEnvelope& envelope) noexcept {
    if constexpr (SessionDiagnosticsEnabled) {
      session_diagnostics_.RecordControlMessage();
    }
    if (!envelope.channel_is_book_ticker) {
      if constexpr (SessionDiagnosticsEnabled) {
        session_diagnostics_.RecordIgnoredTextMessage();
      }
      return;
    }
    if (envelope.has_error || !envelope.result_success) {
      if constexpr (SessionDiagnosticsEnabled) {
        session_diagnostics_.RecordControlError();
      }
      subscription_controller_.MarkUnsubscribeRejected();
      return;
    }
    if constexpr (SessionDiagnosticsEnabled) {
      session_diagnostics_.RecordUnsubscribeAck();
    }
    subscription_controller_.MarkUnsubscribeAccepted();
  }

  websocket::SendStatus SendSubscribe() noexcept {
    last_subscribe_request_ = BuildFuturesBookTickerSubscribeRequest(
        std::span<const std::string_view>(subscription_symbols_.data(),
                                          subscription_symbols_.size()),
        static_cast<std::int64_t>(std::time(nullptr)));
    const websocket::SendStatus status = SendText(last_subscribe_request_);
    if (status == websocket::SendStatus::kOk) {
      if constexpr (SessionDiagnosticsEnabled) {
        session_diagnostics_.RecordSubscribeSent();
      }
    }
    return status;
  }

  websocket::SendStatus SendSubscribeAttempt() noexcept {
    const websocket::SendStatus status = SendSubscribe();
    subscription_controller_.RecordSubscribeAttempt(status);
    if (status != websocket::SendStatus::kOk) {
      if constexpr (SessionDiagnosticsEnabled) {
        session_diagnostics_.RecordSubscribeSendFailure();
      }
    }
    return status;
  }

  websocket::SendStatus SendUnsubscribe() noexcept {
    const std::string request = BuildFuturesBookTickerUnsubscribeRequest(
        std::span<const std::string_view>(subscription_symbols_.data(),
                                          subscription_symbols_.size()),
        static_cast<std::int64_t>(std::time(nullptr)));
    const websocket::SendStatus status = SendText(request);
    subscription_controller_.RecordUnsubscribeAttempt(status);
    if (status == websocket::SendStatus::kOk) {
      if constexpr (SessionDiagnosticsEnabled) {
        session_diagnostics_.RecordUnsubscribeSent();
      }
    }
    return status;
  }

  websocket::SendStatus SendText(std::string_view payload_text) noexcept {
    auto& core = client_.Core();
    const auto payload = std::as_bytes(
        std::span<const char>(payload_text.data(), payload_text.size()));
    return core.SendText(payload);
  }

  std::vector<std::string_view> subscription_symbols_;
  std::string last_subscribe_request_;
  std::atomic<bool> ever_active_{false};
  FuturesMarketDataClient<Consumer, DiagnosticsT, OptionsT> market_data_client_;
  MessageHandler message_handler_;
  Client client_;
  [[no_unique_address]] SessionDiagnosticsT session_diagnostics_{};
  simdjson::ondemand::parser text_parser_;
  BookTickerSubscriptionController subscription_controller_;
  inline static std::atomic<FuturesMarketDataSession*> active_stop_session_{
      nullptr};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_SESSION_H_
