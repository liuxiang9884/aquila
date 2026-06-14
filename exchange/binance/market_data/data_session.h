#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_DATA_SESSION_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_DATA_SESSION_H_

#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/market_data/data_session_diagnostics.h"
#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"
#include "exchange/binance/market_data/client.h"
#include "exchange/binance/market_data/data_session_config.h"
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
  static constexpr websocket::ClockSource kClockSource =
      websocket::ClockSource::kRealtime;
};

struct DefaultPlainWebSocketPolicy : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::PlainSocket;
  static constexpr websocket::ClockSource kClockSource =
      websocket::ClockSource::kRealtime;
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

namespace detail {

inline std::vector<SymbolBinding> BuildOwnedSymbolBindings(
    const std::vector<std::string>& exchange_symbols,
    std::span<const std::int32_t> symbol_ids) {
  std::vector<SymbolBinding> symbols;
  symbols.reserve(exchange_symbols.size());
  for (std::size_t i = 0; i < exchange_symbols.size(); ++i) {
    symbols.push_back(SymbolBinding{
        .symbol = exchange_symbols[i],
        .symbol_id = symbol_ids[i],
    });
  }
  return symbols;
}

}  // namespace detail

template <typename DataSink,
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
              std::span<const SymbolBinding> symbols, DataSink& data_sink,
              ::aquila::market_data::DataSessionLatencyOutlierConfig
                  latency_outlier_config = {})
      : symbol_bindings_(symbols.begin(), symbols.end()),
        stream_target_(
            BuildFuturesBookTickerStreamTarget(std::span<const SymbolBinding>(
                symbol_bindings_.data(), symbol_bindings_.size()))),
        connection_(ApplyOptions(std::move(config), stream_target_)),
        market_data_client_(
            std::span<const SymbolBinding>(symbol_bindings_.data(),
                                           symbol_bindings_.size()),
            data_sink, latency_outlier_config),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(connection_, message_handler_) {
    client_.SetStateHook(this, &HandleState);
  }

  template <size_t N>
  DataSession(websocket::ConnectionConfig config,
              const std::array<SymbolBinding, N>& symbols, DataSink& data_sink,
              ::aquila::market_data::DataSessionLatencyOutlierConfig
                  latency_outlier_config = {})
      : DataSession(std::move(config), std::span<const SymbolBinding>(symbols),
                    data_sink, latency_outlier_config) {}

  DataSession(DataSessionConfig config, DataSink& data_sink)
      : DataSession(std::move(config.name), std::move(config.connection),
                    std::move(config.exchange_symbols),
                    std::move(config.symbol_ids), data_sink,
                    config.diagnostics.latency_outlier) {}

  DataSession(std::string name, websocket::ConnectionConfig config,
              std::vector<std::string> exchange_symbols,
              std::vector<std::int32_t> symbol_ids, DataSink& data_sink,
              ::aquila::market_data::DataSessionLatencyOutlierConfig
                  latency_outlier_config = {})
      : name_(std::move(name)),
        exchange_symbols_(std::move(exchange_symbols)),
        symbol_bindings_(
            detail::BuildOwnedSymbolBindings(exchange_symbols_, symbol_ids)),
        stream_target_(
            BuildFuturesBookTickerStreamTarget(std::span<const SymbolBinding>(
                symbol_bindings_.data(), symbol_bindings_.size()))),
        connection_(ApplyOptions(std::move(config), stream_target_)),
        market_data_client_(
            std::span<const SymbolBinding>(symbol_bindings_.data(),
                                           symbol_bindings_.size()),
            data_sink, latency_outlier_config),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(connection_, message_handler_) {
    client_.SetStateHook(this, &HandleState);
  }

  bool Start() noexcept {
    if (stream_target_.empty()) [[unlikely]] {
      return false;
    }
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

  void OnConnectionPhase(websocket::ConnectionPhase phase) noexcept {
    if (phase == websocket::ConnectionPhase::kActive) {
      ever_active_.store(true, std::memory_order_release);
    }
  }

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
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 2
    const ::aquila::market_data::DataSessionMessageTiming message_timing =
        ::aquila::market_data::MakeDataSessionMessageTiming(view,
                                                            TransportUsesTls);
#endif
    if constexpr (SessionDiagnosticsEnabled) {
      bool decoded_book_ticker = false;
      const websocket::DeliveryResult result =
          market_data_client_.OnTextPayload(payload, view.readable_tail_bytes,
                                            local_ns, &decoded_book_ticker
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 2
                                            ,
                                            &message_timing
#endif
          );
      if (decoded_book_ticker) {
        session_diagnostics_.RecordBookTickerMessage();
      }
      return result;
    } else {
      return market_data_client_.OnTextPayload(
          payload, view.readable_tail_bytes, local_ns
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 2
          ,
          nullptr, &message_timing
#endif
      );
    }
  }

  std::string name_;
  std::vector<std::string> exchange_symbols_;
  std::vector<SymbolBinding> symbol_bindings_;
  std::string stream_target_;
  websocket::ConnectionConfig connection_;
  std::atomic<bool> ever_active_{false};
  FuturesMarketDataClient<DataSink, MarketDataDiagnostics, WebSocketPolicy>
      market_data_client_;
  MessageHandler message_handler_;
  Client client_;
  [[no_unique_address]] SessionDiagnostics session_diagnostics_{};
  inline static std::atomic<DataSession*> active_stop_session_{nullptr};
};

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_DATA_SESSION_H_
