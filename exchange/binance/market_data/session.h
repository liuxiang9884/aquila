#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_SESSION_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_SESSION_H_

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

struct FuturesMarketDataSessionStats {
  std::uint64_t text_messages{0};
  std::uint64_t binary_messages{0};
  std::uint64_t control_messages{0};
  std::uint64_t book_ticker_messages{0};
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

  void RecordBookTickerMessage() noexcept {
    ++stats_.book_ticker_messages;
  }

  [[nodiscard]] const FuturesMarketDataSessionStats& stats() const noexcept {
    return stats_;
  }

 private:
  FuturesMarketDataSessionStats stats_{};
};

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
      : stream_target_(BuildFuturesBookTickerStreamTarget(symbols)),
        market_data_client_(symbols, consumer),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(ApplyOptions(std::move(config), stream_target_),
                message_handler_) {}

  template <size_t N>
  FuturesMarketDataSession(websocket::ConnectionConfig config,
                           const std::array<SymbolBinding, N>& symbols,
                           Consumer& consumer)
      : FuturesMarketDataSession(std::move(config),
                                 std::span<const SymbolBinding>(symbols),
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
  FuturesMarketDataClient<Consumer, DiagnosticsT, OptionsT> market_data_client_;
  MessageHandler message_handler_;
  Client client_;
  [[no_unique_address]] SessionDiagnosticsT session_diagnostics_{};
};

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_SESSION_H_
