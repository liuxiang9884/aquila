#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_CLIENT_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_CLIENT_H_

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
#include "exchange/binance/market_data/book_ticker_parser.h"
#include "exchange/binance/market_data/types.h"

namespace aquila::binance {

struct FuturesMarketDataClientStats {
  std::uint64_t malformed_json_messages{0};
  std::uint64_t book_ticker_messages{0};
  std::uint64_t simdjson_padding_fallback_messages{0};
};

struct NoopFuturesMarketDataDiagnostics {
  static constexpr bool kEnabled = false;
};

namespace detail {

struct NoopBookTickerSlotWriter {
  void operator()(BookTicker&) const noexcept {}
};

inline void AssignBookTickerFromUpdate(const BookTickerUpdate& update,
                                       std::int32_t symbol_id,
                                       std::int64_t local_ns,
                                       BookTicker& out) noexcept {
  out.id = update.update_id;
  out.symbol_id = symbol_id;
  out.exchange = Exchange::kBinance;
  out.exchange_ns = update.event_time_ms * 1'000'000LL;
  out.local_ns = local_ns;
  out.bid_price = update.bid_price;
  out.bid_volume = update.bid_volume;
  out.ask_price = update.ask_price;
  out.ask_volume = update.ask_volume;
}

}  // namespace detail

class FuturesMarketDataDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  void RecordParseDrop(BookTickerParseStatus status) noexcept {
    assert(status == BookTickerParseStatus::kMalformedJson);
    (void)status;
    ++stats_.malformed_json_messages;
  }

  void RecordBookTickerMessage() noexcept {
    ++stats_.book_ticker_messages;
  }

  void RecordSimdjsonPaddingFallback() noexcept {
    ++stats_.simdjson_padding_fallback_messages;
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
    if (view.kind != websocket::PayloadKind::kText) {
      return websocket::DeliveryResult::kAccepted;
    }
    return OnMessage(view,
                     static_cast<std::int64_t>(websocket::NowNs(kClockSource)));
  }

  websocket::DeliveryResult OnMessage(const websocket::MessageView& view,
                                      std::int64_t local_ns) noexcept {
    if (view.kind != websocket::PayloadKind::kText) {
      return websocket::DeliveryResult::kAccepted;
    }
    const std::string_view payload{
        reinterpret_cast<const char*>(view.payload.data()),
        view.payload.size()};
    return OnTextPayload(payload, view.readable_tail_bytes, local_ns);
  }

  websocket::DeliveryResult OnTextPayload(
      std::string_view payload, std::uint32_t readable_tail_bytes,
      std::int64_t local_ns, bool* decoded_book_ticker = nullptr) noexcept {
    if (decoded_book_ticker != nullptr) {
      *decoded_book_ticker = false;
    }

    if constexpr (DiagnosticsEnabled) {
      if (!payload.empty() &&
          readable_tail_bytes < simdjson::SIMDJSON_PADDING) {
        diagnostics_.RecordSimdjsonPaddingFallback();
      }
    }

    BookTickerUpdate update;
    const BookTickerParseStatus status =
        ParseBookTicker(payload, readable_tail_bytes, parser_, update);
    if (status != BookTickerParseStatus::kOk) [[unlikely]] {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseDrop(status);
      }
      return websocket::DeliveryResult::kAccepted;
    }

    const std::int32_t symbol_id = FindSymbolId(update.symbol);

    if constexpr (requires(DataSink& data_sink,
                           detail::NoopBookTickerSlotWriter writer) {
                    data_sink.EmplaceBookTickerWith(writer);
                  }) {
      data_sink_.EmplaceBookTickerWith([&](BookTicker& out) noexcept {
        detail::AssignBookTickerFromUpdate(update, symbol_id, local_ns, out);
      });
    } else {
      BookTicker book_ticker;
      detail::AssignBookTickerFromUpdate(update, symbol_id, local_ns,
                                         book_ticker);
      data_sink_.OnBookTicker(book_ticker);
    }
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordBookTickerMessage();
    }
    if (decoded_book_ticker != nullptr) {
      *decoded_book_ticker = true;
    }
    return websocket::DeliveryResult::kAccepted;
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

  std::int32_t FindSymbolId(std::string_view symbol) const noexcept {
    const auto found = symbol_ids_.find(symbol);
    assert(found != symbol_ids_.end());
    return found->second;
  }

  // Construction-only work; keep it out of the JSON text hot path.
  [[gnu::noinline]] void BuildSymbolLookup(
      std::span<const SymbolBinding> symbols) {
    symbol_ids_.reserve(symbols.size());
    for (const SymbolBinding& binding : symbols) {
      assert(binding.symbol_id >= 0);
      symbol_ids_.emplace(binding.symbol, binding.symbol_id);
    }
  }

  absl::flat_hash_map<std::string_view, std::int32_t> symbol_ids_;
  DataSink& data_sink_;
  simdjson::ondemand::parser parser_;
  [[no_unique_address]] DiagnosticsT diagnostics_{};
};

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_CLIENT_H_
