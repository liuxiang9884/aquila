#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_DATA_SESSION_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_DATA_SESSION_H_

#include <array>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
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
#include "exchange/gate/market_data/data_session_config.h"
#include "exchange/gate/market_data/subscription.h"
#include "exchange/gate/market_data/subscription_controller.h"
#include "exchange/gate/market_data/text_envelope_parser.h"
#include <simdjson.h>

namespace aquila::gate {

struct DataSessionStats {
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

struct NoopDataSessionDiagnostics {
  static constexpr bool kEnabled = false;

  [[nodiscard]] const DataSessionStats& stats() const noexcept {
    return kStats;
  }

 private:
  inline static constexpr DataSessionStats kStats{};
};

class DataSessionDiagnostics {
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

  [[nodiscard]] const DataSessionStats& stats() const noexcept {
    return stats_;
  }

 private:
  DataSessionStats stats_{};
};

namespace detail {

inline std::string BuildDataSessionTarget(const DataSessionConfig& config) {
  std::string target{"/v4/ws/"};
  target.append(config.settle);
  target.append("/sbe?sbe_schema_id=");
  std::array<char, 16> schema_id_buffer{};
  const auto [end, error] = std::to_chars(
      schema_id_buffer.data(),
      schema_id_buffer.data() + schema_id_buffer.size(), config.sbe_schema_id);
  (void)error;
  target.append(schema_id_buffer.data(), end);
  return target;
}

inline void BuildSymbolViews(std::span<const SymbolBinding> symbols,
                             std::vector<std::string_view>* output) {
  output->clear();
  output->reserve(symbols.size());
  for (const SymbolBinding& symbol : symbols) {
    output->push_back(symbol.exchange_symbol);
  }
}

inline std::vector<SymbolBinding> BuildOwnedSymbolBindings(
    const std::vector<std::string>& exchange_symbols,
    std::span<const std::int32_t> symbol_ids) {
  std::vector<SymbolBinding> symbols;
  symbols.reserve(exchange_symbols.size());
  for (std::size_t i = 0; i < exchange_symbols.size(); ++i) {
    symbols.push_back(SymbolBinding{
        .exchange_symbol = exchange_symbols[i],
        .symbol_id = symbol_ids[i],
    });
  }
  return symbols;
}

}  // namespace detail

template <typename Consumer, typename TransportSocketT = websocket::TlsSocket,
          typename DiagnosticsT = NoopFuturesMarketDataDiagnostics,
          typename OptionsT = websocket::DefaultWebSocketOptions,
          typename SessionDiagnosticsT = NoopDataSessionDiagnostics>
class DataSession {
 public:
  using MessageHandler = websocket::MessageHandlerRef<DataSession>;
  using Client =
      websocket::BasicWebSocketClient<TransportSocketT, MessageHandler>;
  static constexpr websocket::ClockSource kClockSource = OptionsT::kClockSource;
  static constexpr bool SessionDiagnosticsEnabled =
      SessionDiagnosticsT::kEnabled;

  DataSession(websocket::ConnectionConfig config,
              std::span<const SymbolBinding> symbols, Consumer& consumer)
      : connection_(ApplyOptions(std::move(config))),
        symbol_bindings_(symbols.begin(), symbols.end()),
        market_data_client_(
            std::span<const SymbolBinding>(symbol_bindings_.data(),
                                           symbol_bindings_.size()),
            consumer),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(connection_, message_handler_) {
    detail::BuildSymbolViews(
        std::span<const SymbolBinding>(symbol_bindings_.data(),
                                       symbol_bindings_.size()),
        &subscription_symbols_);
    client_.SetStateHook(this, &HandleState);
  }

  template <size_t N>
  DataSession(websocket::ConnectionConfig config,
              const std::array<SymbolBinding, N>& symbols, Consumer& consumer)
      : DataSession(std::move(config), std::span<const SymbolBinding>(symbols),
                    consumer) {}

  DataSession(std::string name, websocket::ConnectionConfig config,
              std::vector<std::string> exchange_symbols,
              std::vector<std::int32_t> symbol_ids, Consumer& consumer)
      : name_(std::move(name)),
        connection_(ApplyOptions(std::move(config))),
        exchange_symbols_(std::move(exchange_symbols)),
        symbol_bindings_(
            detail::BuildOwnedSymbolBindings(exchange_symbols_, symbol_ids)),
        market_data_client_(
            std::span<const SymbolBinding>(symbol_bindings_.data(),
                                           symbol_bindings_.size()),
            consumer),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(connection_, message_handler_) {
    detail::BuildSymbolViews(
        std::span<const SymbolBinding>(symbol_bindings_.data(),
                                       symbol_bindings_.size()),
        &subscription_symbols_);
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

  [[nodiscard]] std::string_view name() const noexcept {
    return name_;
  }

  [[nodiscard]] const websocket::ConnectionConfig& connection() const noexcept {
    return connection_;
  }

  [[nodiscard]] std::span<const SymbolBinding> symbols() const noexcept {
    return {symbol_bindings_.data(), symbol_bindings_.size()};
  }

  [[nodiscard]] bool ever_active() const noexcept {
    return ever_active_.load(std::memory_order_acquire);
  }

  [[nodiscard]] const DataSessionStats& stats() const noexcept {
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
    explicit ScopedStopHandlers(DataSession* session) noexcept
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
      DataSession* expected = session_;
      (void)active_stop_session_.compare_exchange_strong(
          expected, nullptr, std::memory_order_acq_rel,
          std::memory_order_acquire);
    }

   private:
    using SignalHandler = void (*)(int);

    DataSession* session_{nullptr};
    SignalHandler previous_int_handler_{SIG_DFL};
    SignalHandler previous_term_handler_{SIG_DFL};
  };

  static void HandleStopSignal(int signal) noexcept {
    if (signal != SIGINT && signal != SIGTERM) {
      return;
    }
    DataSession* session = active_stop_session_.load(std::memory_order_acquire);
    if (session != nullptr) {
      session->Stop();
    }
  }

  static void HandleState(void* context,
                          websocket::ConnectionPhase phase) noexcept {
    static_cast<DataSession*>(context)->OnConnectionPhase(phase);
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

  std::string name_;
  websocket::ConnectionConfig connection_;
  std::vector<std::string> exchange_symbols_;
  std::vector<SymbolBinding> symbol_bindings_;
  std::vector<std::string_view> subscription_symbols_;
  std::string last_subscribe_request_;
  std::atomic<bool> ever_active_{false};
  FuturesMarketDataClient<Consumer, DiagnosticsT, OptionsT> market_data_client_;
  MessageHandler message_handler_;
  Client client_;
  [[no_unique_address]] SessionDiagnosticsT session_diagnostics_{};
  simdjson::ondemand::parser text_parser_;
  BookTickerSubscriptionController subscription_controller_;
  inline static std::atomic<DataSession*> active_stop_session_{nullptr};
};

template <typename Consumer, typename TransportSocketT = websocket::TlsSocket,
          typename DiagnosticsT = NoopFuturesMarketDataDiagnostics,
          typename OptionsT = websocket::DefaultWebSocketOptions,
          typename SessionDiagnosticsT = NoopDataSessionDiagnostics>
struct DataSessionCreateResult {
  using Session = DataSession<Consumer, TransportSocketT, DiagnosticsT,
                              OptionsT, SessionDiagnosticsT>;

  std::unique_ptr<Session> session;
  std::string error;
  bool ok{false};
};

template <typename Consumer, typename TransportSocketT = websocket::TlsSocket,
          typename DiagnosticsT = NoopFuturesMarketDataDiagnostics,
          typename OptionsT = websocket::DefaultWebSocketOptions,
          typename SessionDiagnosticsT = NoopDataSessionDiagnostics>
[[nodiscard]] DataSessionCreateResult<Consumer, TransportSocketT, DiagnosticsT,
                                      OptionsT, SessionDiagnosticsT>
CreateDataSession(const DataSessionConfig& session_config,
                  const config::InstrumentCatalog& catalog,
                  Consumer& consumer) {
  using Result =
      DataSessionCreateResult<Consumer, TransportSocketT, DiagnosticsT,
                              OptionsT, SessionDiagnosticsT>;
  Result result;
  if (session_config.feed != "book_ticker" ||
      session_config.wire_format != "sbe") {
    result.error = "Gate data session supports only SBE book_ticker";
    return result;
  }

  config::ConnectionConfigResult connection_result = config::ToConnectionConfig(
      session_config.websocket, detail::BuildDataSessionTarget(session_config));
  if (!connection_result.ok) {
    result.error = std::move(connection_result.error);
    return result;
  }

  std::vector<std::string> exchange_symbols;
  exchange_symbols.reserve(session_config.subscribe_symbols.size());
  std::vector<std::int32_t> symbol_ids;
  symbol_ids.reserve(session_config.subscribe_symbols.size());
  for (const std::string& symbol : session_config.subscribe_symbols) {
    const config::InstrumentInfo* info = catalog.Find(Exchange::kGate, symbol);
    if (info == nullptr) {
      result.error = "Gate instrument not found: ";
      result.error.append(symbol);
      return result;
    }
    exchange_symbols.push_back(info->exchange_symbol);
    symbol_ids.push_back(info->symbol_id);
  }

  result.session = std::make_unique<typename Result::Session>(
      session_config.name, std::move(connection_result.config),
      std::move(exchange_symbols), std::move(symbol_ids), consumer);
  result.ok = true;
  return result;
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_DATA_SESSION_H_
