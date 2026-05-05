#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_DATA_SESSION_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_DATA_SESSION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"
#include "exchange/binance/market_data/client.h"
#include "exchange/binance/market_data/stream.h"

namespace aquila::binance {

struct DataSessionStats {
  std::uint64_t text_messages{0};
  std::uint64_t binary_messages{0};
  std::uint64_t control_messages{0};
  std::uint64_t book_ticker_messages{0};
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

  void RecordBookTickerMessage() noexcept {
    ++stats_.book_ticker_messages;
  }

  [[nodiscard]] const DataSessionStats& stats() const noexcept {
    return stats_;
  }

 private:
  DataSessionStats stats_{};
};

struct DefaultTlsWebSocketPolicy : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::TlsSocket;
};

struct DefaultPlainWebSocketPolicy : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::PlainSocket;
};

struct NoopDataSessionDiagnosticsPolicy {
  using MarketDataDiagnostics = NoopFuturesMarketDataDiagnostics;
  using SessionDiagnostics = NoopDataSessionDiagnostics;
};

struct SessionOnlyDiagnosticsPolicy {
  using MarketDataDiagnostics = NoopFuturesMarketDataDiagnostics;
  using SessionDiagnostics = DataSessionDiagnostics;
};

struct DataSessionDiagnosticsPolicy {
  using MarketDataDiagnostics = FuturesMarketDataDiagnostics;
  using SessionDiagnostics = DataSessionDiagnostics;
};

template <typename Consumer,
          typename WebSocketPolicy = DefaultTlsWebSocketPolicy,
          typename DiagnosticsPolicy = NoopDataSessionDiagnosticsPolicy>
class DataSession {
 public:
  using TransportSocket = typename WebSocketPolicy::TransportSocket;
  using MarketDataDiagnostics =
      typename DiagnosticsPolicy::MarketDataDiagnostics;
  using SessionDiagnostics = typename DiagnosticsPolicy::SessionDiagnostics;
  using MessageHandler = websocket::MessageHandlerRef<DataSession>;
  using Client =
      websocket::BasicWebSocketClient<TransportSocket, MessageHandler>;
  static constexpr bool TransportUsesTls = TransportSocket::kUsesTls;
  static constexpr websocket::ClockSource kClockSource =
      WebSocketPolicy::kClockSource;
  static constexpr bool SessionDiagnosticsEnabled =
      SessionDiagnostics::kEnabled;

  DataSession(websocket::ConnectionConfig config,
              std::span<const SymbolBinding> symbols, Consumer& consumer)
      : stream_target_(BuildFuturesBookTickerStreamTarget(symbols)),
        market_data_client_(symbols, consumer),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(ApplyOptions(std::move(config), stream_target_),
                message_handler_) {}

  template <size_t N>
  DataSession(websocket::ConnectionConfig config,
              const std::array<SymbolBinding, N>& symbols, Consumer& consumer)
      : DataSession(std::move(config), std::span<const SymbolBinding>(symbols),
                    consumer) {}

  bool Start() noexcept {
    if (stream_target_.empty()) [[unlikely]] {
      return false;
    }
    return client_.Start();
  }

  void Stop() noexcept {
    client_.Stop();
  }

  void SetStateHandler(void* context,
                       websocket::StateHandler handler) noexcept {
    client_.SetStateHandler(context, handler);
  }

  void SetErrorHandler(void* context,
                       websocket::ErrorHandler handler) noexcept {
    client_.SetErrorHandler(context, handler);
  }

  websocket::DeliveryResult Handle(
      const websocket::MessageView& view) noexcept {
    switch (view.kind) {
      case websocket::PayloadKind::kText:
        return HandleText(view);
      case websocket::PayloadKind::kBinary:
        if constexpr (SessionDiagnosticsEnabled) {
          session_diagnostics_.RecordBinaryMessage();
        }
        return websocket::DeliveryResult::kAccepted;
      case websocket::PayloadKind::kPing:
      case websocket::PayloadKind::kPong:
      case websocket::PayloadKind::kClose:
        if constexpr (SessionDiagnosticsEnabled) {
          session_diagnostics_.RecordControlMessage();
        }
        return websocket::DeliveryResult::kAccepted;
    }
    return websocket::DeliveryResult::kAccepted;
  }

  [[nodiscard]] websocket::ConnectionPhase phase() const noexcept {
    return client_.phase();
  }

  [[nodiscard]] websocket::ConnectionError last_error() const noexcept {
    return client_.last_error();
  }

  [[nodiscard]] const DataSessionStats& stats() const noexcept {
    return session_diagnostics_.stats();
  }

  [[nodiscard]] const MarketDataDiagnostics& market_data_client_diagnostics()
      const noexcept {
    return market_data_client_.diagnostics();
  }

  [[nodiscard]] websocket::Metrics SnapshotMetrics() const noexcept {
    return client_.SnapshotMetrics();
  }

  [[nodiscard]] int NativeFd() noexcept {
    return client_.Core().NativeFd();
  }

  [[nodiscard]] std::string_view stream_target() const noexcept {
    return stream_target_;
  }

 private:
  static websocket::ConnectionConfig ApplyOptions(
      websocket::ConnectionConfig config, std::string_view stream_target) {
    config.runtime_policy.clock_source = kClockSource;
    config.target = std::string(stream_target);
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
    const std::int64_t local_ns =
        static_cast<std::int64_t>(websocket::NowNs(kClockSource));
    if constexpr (SessionDiagnosticsEnabled) {
      bool decoded_book_ticker = false;
      const websocket::DeliveryResult result =
          market_data_client_.OnTextPayload(payload, view.readable_tail_bytes,
                                            local_ns, &decoded_book_ticker);
      if (decoded_book_ticker) {
        session_diagnostics_.RecordBookTickerMessage();
      }
      return result;
    } else {
      return market_data_client_.OnTextPayload(
          payload, view.readable_tail_bytes, local_ns);
    }
  }

  std::string stream_target_;
  FuturesMarketDataClient<Consumer, MarketDataDiagnostics, WebSocketPolicy>
      market_data_client_;
  MessageHandler message_handler_;
  Client client_;
  [[no_unique_address]] SessionDiagnostics session_diagnostics_{};
};

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_DATA_SESSION_H_
