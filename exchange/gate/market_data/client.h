#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_CLIENT_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_CLIENT_H_

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <absl/container/flat_hash_map.h>

#include "core/market_data/types.h"
#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "exchange/gate/sbe/book_ticker_decoder.h"
#include "exchange/gate/sbe/message_dispatcher.h"

namespace aquila::gate {

struct SymbolBinding {
  std::string_view exchange_symbol{};
  std::int32_t symbol_id{-1};
};

struct FuturesMarketDataClientStats {
  std::uint64_t sbe_need_more{0};
  std::uint64_t unsupported_sbe_schemas{0};
  std::uint64_t unsupported_sbe_schema_versions{0};
  std::uint64_t unsupported_sbe_templates{0};
  std::uint64_t unsupported_sbe_messages{0};
};

struct NoopFuturesMarketDataDiagnostics {
  static constexpr bool kEnabled = false;
};

namespace detail {

struct NoopBookTickerSlotWriter {
  void operator()(BookTicker&) const noexcept {}
};

}  // namespace detail

class FuturesMarketDataDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  void RecordDispatchDrop(SbeDispatchStatus status) noexcept {
    switch (status) {
      case SbeDispatchStatus::kNeedMore:
        ++stats_.sbe_need_more;
        return;
      case SbeDispatchStatus::kUnsupportedSchema:
        ++stats_.unsupported_sbe_schemas;
        return;
      case SbeDispatchStatus::kUnsupportedSchemaVersion:
        ++stats_.unsupported_sbe_schema_versions;
        return;
      case SbeDispatchStatus::kUnsupportedTemplate:
        ++stats_.unsupported_sbe_templates;
        return;
      case SbeDispatchStatus::kReady:
        return;
    }
  }

  void RecordUnsupportedSbeMessage() noexcept {
    ++stats_.unsupported_sbe_messages;
  }

  [[nodiscard]] const FuturesMarketDataClientStats& stats() const noexcept {
    return stats_;
  }

 private:
  FuturesMarketDataClientStats stats_{};
};

template <typename DataSink,
          typename DiagnosticsT = NoopFuturesMarketDataDiagnostics,
          typename OptionsT = websocket::DefaultWebSocketOptions>
class FuturesMarketDataClient {
 public:
  static constexpr bool DiagnosticsEnabled = DiagnosticsT::kEnabled;
  static constexpr websocket::ClockSource kClockSource = OptionsT::kClockSource;

  FuturesMarketDataClient(std::span<const SymbolBinding> symbols,
                          DataSink& data_sink)
      : data_sink_(data_sink) {
    BuildSymbolLookup(symbols);
  }

  template <size_t N>
  FuturesMarketDataClient(const std::array<SymbolBinding, N>& symbols,
                          DataSink& data_sink)
      : FuturesMarketDataClient(std::span<const SymbolBinding>(symbols),
                                data_sink) {}

  websocket::MessageCallback AsMessageCallback() noexcept {
    return {.context = this, .handler = &HandleWebSocketMessage};
  }

  websocket::DeliveryResult Handle(
      const websocket::MessageView& view) noexcept {
    return OnMessage(view);
  }

  websocket::DeliveryResult OnMessage(
      const websocket::MessageView& view) noexcept {
    return OnMessage(view,
                     static_cast<std::int64_t>(websocket::NowNs(kClockSource)));
  }

  websocket::DeliveryResult OnMessage(const websocket::MessageView& view,
                                      std::int64_t local_ns) noexcept {
    if (view.kind != websocket::PayloadKind::kBinary) {
      return websocket::DeliveryResult::kAccepted;
    }

    return OnBinaryPayload(view.payload, local_ns);
  }

  websocket::DeliveryResult OnBinaryPayload(std::span<const std::byte> payload,
                                            std::int64_t local_ns) noexcept {
    const std::string_view payload_view{
        reinterpret_cast<const char*>(payload.data()), payload.size()};
    const SbeDispatchResult dispatch = DispatchSbeMessage(payload_view);
    if (dispatch.status != SbeDispatchStatus::kReady) [[unlikely]] {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordDispatchDrop(dispatch.status);
      }
      return websocket::DeliveryResult::kAccepted;
    }

    switch (dispatch.message_type) {
      case GateSbeMessageType::kBookTicker:
        return OnBookTickerPayload(payload_view, dispatch.header, local_ns);
      default:
        if constexpr (DiagnosticsEnabled) {
          diagnostics_.RecordUnsupportedSbeMessage();
        }
        return websocket::DeliveryResult::kAccepted;
    }
  }

  [[nodiscard]] const DiagnosticsT& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  static websocket::DeliveryResult HandleWebSocketMessage(
      void* context, const websocket::MessageView& view) noexcept {
    assert(context != nullptr);
    return static_cast<FuturesMarketDataClient*>(context)->OnMessage(view);
  }

  websocket::DeliveryResult OnBookTickerPayload(
      std::string_view payload, const SbeMessageHeader& header,
      std::int64_t local_ns) noexcept {
    const std::string_view exchange_symbol =
        ExtractTrustedBookTickerSymbol(payload, header);
    const std::int32_t symbol_id = FindSymbolId(exchange_symbol);

    if constexpr (requires(DataSink& data_sink,
                           detail::NoopBookTickerSlotWriter writer) {
                    data_sink.EmplaceBookTickerWith(writer);
                  }) {
      data_sink_.EmplaceBookTickerWith([&](BookTicker& out) noexcept {
        DecodeTrustedBookTickerWithHeader(payload, header, local_ns, symbol_id,
                                          out);
      });
    } else {
      BookTicker book_ticker;
      DecodeTrustedBookTickerWithHeader(payload, header, local_ns, symbol_id,
                                        book_ticker);
      data_sink_.OnBookTicker(book_ticker);
    }
    return websocket::DeliveryResult::kAccepted;
  }

  std::int32_t FindSymbolId(std::string_view exchange_symbol) const noexcept {
    const auto found = symbol_ids_by_exchange_symbol_.find(exchange_symbol);
    assert(found != symbol_ids_by_exchange_symbol_.end());
    return found->second;
  }

  // This runs during construction; keep it out-of-line so message handlers
  // keep their hot-path inlining budget.
  [[gnu::noinline]] void BuildSymbolLookup(
      std::span<const SymbolBinding> symbols) {
    symbol_ids_by_exchange_symbol_.reserve(symbols.size());
    for (const SymbolBinding& binding : symbols) {
      assert(binding.symbol_id >= 0);
      symbol_ids_by_exchange_symbol_.emplace(binding.exchange_symbol,
                                             binding.symbol_id);
    }
  }

  absl::flat_hash_map<std::string_view, std::int32_t>
      symbol_ids_by_exchange_symbol_;
  DataSink& data_sink_;
  [[no_unique_address]] DiagnosticsT diagnostics_{};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_CLIENT_H_
