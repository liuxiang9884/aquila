#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_CLIENT_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_CLIENT_H_

#include <array>
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
  std::uint64_t missing_fields{0};
  std::uint64_t invalid_numbers{0};
  std::uint64_t unsupported_events{0};
  std::uint64_t symbol_too_long{0};
  std::uint64_t unknown_symbols{0};
  std::uint64_t book_ticker_messages{0};
};

struct NoopFuturesMarketDataDiagnostics {
  static constexpr bool kEnabled = false;
};

class FuturesMarketDataDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  void RecordParseDrop(BookTickerParseStatus status) noexcept {
    switch (status) {
      case BookTickerParseStatus::kMalformedJson:
        ++stats_.malformed_json_messages;
        return;
      case BookTickerParseStatus::kMissingField:
        ++stats_.missing_fields;
        return;
      case BookTickerParseStatus::kInvalidNumber:
        ++stats_.invalid_numbers;
        return;
      case BookTickerParseStatus::kUnsupportedEvent:
        ++stats_.unsupported_events;
        return;
      case BookTickerParseStatus::kSymbolTooLong:
        ++stats_.symbol_too_long;
        return;
      case BookTickerParseStatus::kOk:
        return;
    }
  }

  void RecordUnknownSymbol() noexcept {
    ++stats_.unknown_symbols;
  }

  void RecordBookTickerMessage() noexcept {
    ++stats_.book_ticker_messages;
  }

  [[nodiscard]] const FuturesMarketDataClientStats& stats() const noexcept {
    return stats_;
  }

 private:
  FuturesMarketDataClientStats stats_{};
};

template <typename Consumer,
          typename DiagnosticsT = NoopFuturesMarketDataDiagnostics,
          typename OptionsT = websocket::DefaultWebSocketOptions,
          typename BookTickerParserT = SimdjsonBookTickerParser>
class FuturesMarketDataClient {
 public:
  static constexpr bool DiagnosticsEnabled = DiagnosticsT::kEnabled;
  static constexpr websocket::ClockSource kClockSource = OptionsT::kClockSource;

  FuturesMarketDataClient(std::span<const SymbolBinding> symbols,
                          Consumer& consumer)
      : symbols_(symbols), consumer_(consumer) {
    BuildSymbolLookup();
  }

  template <size_t N>
  FuturesMarketDataClient(const std::array<SymbolBinding, N>& symbols,
                          Consumer& consumer)
      : FuturesMarketDataClient(std::span<const SymbolBinding>(symbols),
                                consumer) {}

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
    if (view.kind != websocket::PayloadKind::kText || !view.fin) {
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

    BookTickerUpdate update{};
    const BookTickerParseStatus status =
        book_ticker_parser_.Parse(payload, readable_tail_bytes, update);
    if (status != BookTickerParseStatus::kOk) [[unlikely]] {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseDrop(status);
      }
      return websocket::DeliveryResult::kAccepted;
    }

    const std::int32_t symbol_id = FindSymbolId(update.symbol);
    if (symbol_id < 0) [[unlikely]] {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordUnknownSymbol();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    const BookTicker book_ticker{
        .id = update.update_id,
        .symbol_id = symbol_id,
        .exchange = Exchange::kBinance,
        .exchange_ns = update.event_time_ms * 1'000'000LL,
        .local_ns = local_ns,
        .bid_price = update.bid_price,
        .bid_volume = update.bid_volume,
        .ask_price = update.ask_price,
        .ask_volume = update.ask_volume,
    };
    consumer_.OnBookTicker(book_ticker);
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
    if (context == nullptr) {
      return websocket::DeliveryResult::kFatal;
    }
    return static_cast<FuturesMarketDataClient*>(context)->OnMessage(view);
  }

  std::int32_t FindSymbolId(std::string_view symbol) const noexcept {
    const auto found = symbol_ids_.find(symbol);
    return found == symbol_ids_.end() ? -1 : found->second;
  }

  // Construction-only work; keep it out of the JSON text hot path.
  [[gnu::noinline]] void BuildSymbolLookup() {
    symbol_ids_.reserve(symbols_.size());
    for (const SymbolBinding& binding : symbols_) {
      if (binding.symbol_id >= 0) {
        symbol_ids_.emplace(binding.symbol, binding.symbol_id);
      }
    }
  }

  std::span<const SymbolBinding> symbols_;
  absl::flat_hash_map<std::string_view, std::int32_t> symbol_ids_;
  Consumer& consumer_;
  BookTickerParserT book_ticker_parser_;
  [[no_unique_address]] DiagnosticsT diagnostics_{};
};

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_CLIENT_H_
