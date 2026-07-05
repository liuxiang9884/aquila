#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_CLIENT_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_CLIENT_H_

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <absl/container/flat_hash_map.h>

#include "core/common/data_session_diagnostic_level.h"
#include "core/market_data/data_session_diagnostics.h"
#include "core/market_data/types.h"
#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "exchange/binance/market_data/book_ticker_parser.h"
#include "exchange/binance/market_data/trade_parser.h"
#include "exchange/binance/market_data/types.h"

namespace aquila::binance {

struct FuturesMarketDataClientStats {
  std::uint64_t malformed_json_messages{0};
  std::uint64_t unsupported_event_messages{0};
  std::uint64_t book_ticker_messages{0};
  std::uint64_t trade_messages{0};
  std::uint64_t simdjson_padding_fallback_messages{0};
};

struct NoopFuturesMarketDataDiagnostics {
  static constexpr bool kEnabled = false;
};

namespace detail {

struct NoopBookTickerSlotWriter {
  void operator()(BookTicker&) const noexcept {}
};

struct NoopTradeSlotWriter {
  void operator()(Trade&) const noexcept {}
};

enum class MarketDataEventKind : std::uint8_t {
  kBookTicker = 0,
  kTrade,
  kUnsupported,
  kMalformedJson,
};

struct ParsedMarketDataEvent {
  MarketDataEventKind kind{MarketDataEventKind::kMalformedJson};
  BookTickerUpdate book_ticker;
  TradeUpdate trade;
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

inline void AssignTradeFromUpdate(const TradeUpdate& update,
                                  std::int32_t symbol_id, std::int64_t local_ns,
                                  Trade& out) noexcept {
  out.id = update.trade_id;
  out.symbol_id = symbol_id;
  out.exchange = Exchange::kBinance;
  out.side = update.buyer_is_maker ? OrderSide::kSell : OrderSide::kBuy;
  out.reserved = 0;
  out.exchange_ns = update.event_time_ms * 1'000'000LL;
  out.trade_ns = update.trade_time_ms * 1'000'000LL;
  out.local_ns = local_ns;
  out.price = update.price;
  out.volume = update.volume;
  out.batch_index = 0;
  out.batch_count = 1;
}

inline MarketDataEventKind EventKindFromText(std::string_view event) noexcept {
  if (event == "bookTicker") {
    return MarketDataEventKind::kBookTicker;
  }
  if (event == "trade") {
    return MarketDataEventKind::kTrade;
  }
  return MarketDataEventKind::kUnsupported;
}

inline bool TryReadEventKind(simdjson::ondemand::object& root,
                             MarketDataEventKind* kind) noexcept {
  simdjson::simdjson_result<simdjson::ondemand::value> event_value =
      root.find_field_unordered("e");
  if (event_value.error() != simdjson::SUCCESS) {
    return false;
  }
  simdjson::simdjson_result<std::string_view> event_text =
      event_value.value_unsafe().get_string();
  if (event_text.error() != simdjson::SUCCESS) {
    return false;
  }
  *kind = EventKindFromText(event_text.value_unsafe());
  return true;
}

inline MarketDataEventKind ParseMarketDataObject(
    simdjson::ondemand::object root, DataSessionFeeds feeds,
    ParsedMarketDataEvent& output) noexcept {
  MarketDataEventKind kind{MarketDataEventKind::kMalformedJson};
  if (!TryReadEventKind(root, &kind)) {
    output.kind = MarketDataEventKind::kMalformedJson;
    return MarketDataEventKind::kMalformedJson;
  }
  output.kind = kind;
  switch (kind) {
    case MarketDataEventKind::kBookTicker:
      if (!feeds.book_ticker) {
        return MarketDataEventKind::kUnsupported;
      }
      return ParseBookTickerObject(root, output.book_ticker) ==
                     BookTickerParseStatus::kOk
                 ? MarketDataEventKind::kBookTicker
                 : MarketDataEventKind::kMalformedJson;
    case MarketDataEventKind::kTrade:
      if (!feeds.trade) {
        return MarketDataEventKind::kUnsupported;
      }
      return ParseTradeObject(root, output.trade) == TradeParseStatus::kOk
                 ? MarketDataEventKind::kTrade
                 : MarketDataEventKind::kMalformedJson;
    case MarketDataEventKind::kUnsupported:
    case MarketDataEventKind::kMalformedJson:
      return kind;
  }
  return MarketDataEventKind::kUnsupported;
}

inline MarketDataEventKind ParseMarketDataDocument(
    simdjson::ondemand::document document, DataSessionFeeds feeds,
    ParsedMarketDataEvent& output) noexcept {
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    output.kind = MarketDataEventKind::kMalformedJson;
    return MarketDataEventKind::kMalformedJson;
  }
  return ParseMarketDataObject(root, feeds, output);
}

inline MarketDataEventKind ParseMarketDataEvent(
    std::string_view payload, std::uint32_t readable_tail_bytes,
    DataSessionFeeds feeds, simdjson::ondemand::parser& parser,
    ParsedMarketDataEvent& output) noexcept {
  if (payload.empty()) {
    output.kind = MarketDataEventKind::kMalformedJson;
    return MarketDataEventKind::kMalformedJson;
  }

  simdjson::ondemand::document document;
  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    simdjson::padded_string_view view(payload.data(), payload.size(),
                                      payload.size() + readable_tail_bytes);
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      output.kind = MarketDataEventKind::kMalformedJson;
      return MarketDataEventKind::kMalformedJson;
    }
    return ParseMarketDataDocument(std::move(document), feeds, output);
  }

  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    output.kind = MarketDataEventKind::kMalformedJson;
    return MarketDataEventKind::kMalformedJson;
  }
  return ParseMarketDataDocument(std::move(document), feeds, output);
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

  void RecordMalformedJsonMessage() noexcept {
    ++stats_.malformed_json_messages;
  }

  void RecordUnsupportedEventMessage() noexcept {
    ++stats_.unsupported_event_messages;
  }

  void RecordBookTickerMessage() noexcept {
    ++stats_.book_ticker_messages;
  }

  void RecordTradeMessage() noexcept {
    ++stats_.trade_messages;
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
                          DataSink& data_sink,
                          ::aquila::market_data::DataSessionLatencyOutlierConfig
                              latency_outlier_config = {})
      : FuturesMarketDataClient(
            symbols, DataSessionFeeds{.book_ticker = true, .trade = true},
            data_sink, latency_outlier_config) {}

  FuturesMarketDataClient(std::span<const SymbolBinding> symbols,
                          DataSessionFeeds feeds, DataSink& data_sink,
                          ::aquila::market_data::DataSessionLatencyOutlierConfig
                              latency_outlier_config = {})
      : data_sink_(data_sink),
        feeds_(EffectiveFeeds(feeds, data_sink))
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 1
        ,
        latency_outlier_logger_(latency_outlier_config)
#endif
  {
#if AQUILA_DATA_SESSION_DIAG_LEVEL < 1
    (void)latency_outlier_config;
#endif
    BuildSymbolLookup(symbols);
  }

  template <size_t N>
  FuturesMarketDataClient(const std::array<SymbolBinding, N>& symbols,
                          DataSink& data_sink,
                          ::aquila::market_data::DataSessionLatencyOutlierConfig
                              latency_outlier_config = {})
      : FuturesMarketDataClient(std::span<const SymbolBinding>(symbols),
                                data_sink, latency_outlier_config) {}

  template <size_t N>
  FuturesMarketDataClient(const std::array<SymbolBinding, N>& symbols,
                          DataSessionFeeds feeds, DataSink& data_sink,
                          ::aquila::market_data::DataSessionLatencyOutlierConfig
                              latency_outlier_config = {})
      : FuturesMarketDataClient(std::span<const SymbolBinding>(symbols), feeds,
                                data_sink, latency_outlier_config) {}

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
      std::int64_t local_ns, bool* decoded_book_ticker = nullptr,
      bool* decoded_trade = nullptr
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 2
      ,
      const ::aquila::market_data::DataSessionMessageTiming* message_timing =
          nullptr
#endif
      ) noexcept {
    if (decoded_book_ticker != nullptr) {
      *decoded_book_ticker = false;
    }
    if (decoded_trade != nullptr) {
      *decoded_trade = false;
    }

    if constexpr (DiagnosticsEnabled) {
      if (!payload.empty() &&
          readable_tail_bytes < simdjson::SIMDJSON_PADDING) {
        diagnostics_.RecordSimdjsonPaddingFallback();
      }
    }

    detail::ParsedMarketDataEvent event;
    const detail::MarketDataEventKind kind = detail::ParseMarketDataEvent(
        payload, readable_tail_bytes, feeds_, parser_, event);
    if (kind == detail::MarketDataEventKind::kMalformedJson) [[unlikely]] {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordMalformedJsonMessage();
      }
      return websocket::DeliveryResult::kAccepted;
    }
    if (kind == detail::MarketDataEventKind::kUnsupported) [[unlikely]] {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordUnsupportedEventMessage();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    if (kind == detail::MarketDataEventKind::kTrade) {
      return OnTradeEvent(event.trade, local_ns, decoded_trade);
    }

    const BookTickerUpdate& update = event.book_ticker;

    const std::int32_t symbol_id = FindSymbolId(update.symbol);

#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 1
    BookTicker book_ticker;
    detail::AssignBookTickerFromUpdate(update, symbol_id, local_ns,
                                       book_ticker);
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 2
    const std::int64_t parse_done_ns =
        static_cast<std::int64_t>(websocket::NowNs(kClockSource));
#endif
    PublishDecodedBookTicker(book_ticker);
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 2
    const std::int64_t shm_publish_done_ns =
        static_cast<std::int64_t>(websocket::NowNs(kClockSource));
#endif
    ::aquila::market_data::DataSessionBookTickerTiming timing{
        .exchange = Exchange::kBinance,
        .source_id = latency_outlier_logger_.config().source_id,
        .symbol_id = book_ticker.symbol_id,
        .book_ticker_id = book_ticker.id,
        .exchange_ns = book_ticker.exchange_ns,
        .book_ticker_local_ns = book_ticker.local_ns,
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 2
        .parse_done_ns = parse_done_ns,
        .shm_publish_done_ns = shm_publish_done_ns,
#endif
    };
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 2
    if (message_timing != nullptr) {
      timing.message = *message_timing;
    }
#endif
    latency_outlier_logger_.MaybeLog(timing);
#else
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
#endif
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

  static DataSessionFeeds EffectiveFeeds(DataSessionFeeds feeds,
                                         const DataSink& data_sink) noexcept {
    if constexpr (requires { data_sink.has_book_ticker_channel(); }) {
      feeds.book_ticker =
          feeds.book_ticker && data_sink.has_book_ticker_channel();
    }
    if constexpr (requires { data_sink.has_trade_channel(); }) {
      feeds.trade = feeds.trade && data_sink.has_trade_channel();
    }
    return feeds;
  }

  websocket::DeliveryResult OnTradeEvent(const TradeUpdate& update,
                                         std::int64_t local_ns,
                                         bool* decoded_trade) noexcept {
    const std::int32_t symbol_id = FindSymbolId(update.symbol);
    if constexpr (requires(DataSink& data_sink,
                           detail::NoopTradeSlotWriter writer) {
                    data_sink.EmplaceTradeWith(writer);
                  }) {
      data_sink_.EmplaceTradeWith([&](Trade& out) noexcept {
        detail::AssignTradeFromUpdate(update, symbol_id, local_ns, out);
      });
    } else {
      if constexpr (requires(DataSink& data_sink, const Trade& trade) {
                      data_sink.OnTrade(trade);
                    }) {
        Trade trade;
        detail::AssignTradeFromUpdate(update, symbol_id, local_ns, trade);
        data_sink_.OnTrade(trade);
      }
    }
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordTradeMessage();
    }
    if (decoded_trade != nullptr) {
      *decoded_trade = true;
    }
    return websocket::DeliveryResult::kAccepted;
  }

#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 1
  void PublishDecodedBookTicker(const BookTicker& book_ticker) noexcept {
    if constexpr (requires(DataSink& data_sink,
                           detail::NoopBookTickerSlotWriter writer) {
                    data_sink.EmplaceBookTickerWith(writer);
                  }) {
      data_sink_.EmplaceBookTickerWith(
          [&](BookTicker& out) noexcept { out = book_ticker; });
    } else {
      data_sink_.OnBookTicker(book_ticker);
    }
  }
#endif

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
  DataSessionFeeds feeds_;
  simdjson::ondemand::parser parser_;
  [[no_unique_address]] DiagnosticsT diagnostics_{};
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 1
  ::aquila::market_data::DataSessionLatencyOutlierLogger
      latency_outlier_logger_{};
#endif
};

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_CLIENT_H_
