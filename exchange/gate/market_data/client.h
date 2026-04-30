#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_CLIENT_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_CLIENT_H_

#include <array>
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
  std::string_view symbol{};
  std::int32_t symbol_id{-1};
};

struct FuturesMarketDataClientStats {
  std::uint64_t sbe_need_more{0};
  std::uint64_t unsupported_sbe_schemas{0};
  std::uint64_t unsupported_sbe_schema_versions{0};
  std::uint64_t unsupported_sbe_templates{0};
  std::uint64_t unsupported_sbe_messages{0};
  std::uint64_t unknown_symbols{0};
  std::uint64_t book_ticker_decode_failures{0};
};

template <typename Consumer>
class FuturesMarketDataClient {
 public:
  FuturesMarketDataClient(
      std::span<const SymbolBinding> symbols, Consumer& consumer,
      websocket::ClockSource clock_source = websocket::ClockSource::kSteady)
      : symbols_(symbols), consumer_(consumer), clock_source_(clock_source) {
    BuildSymbolLookup();
  }

  template <size_t N>
  FuturesMarketDataClient(
      const std::array<SymbolBinding, N>& symbols, Consumer& consumer,
      websocket::ClockSource clock_source = websocket::ClockSource::kSteady)
      : FuturesMarketDataClient(std::span<const SymbolBinding>(symbols),
                                consumer, clock_source) {}

  websocket::MessageCallback AsMessageCallback() noexcept {
    return {.context = this, .handler = &HandleWebSocketMessage};
  }

  websocket::DeliveryResult Handle(
      const websocket::MessageView& view) noexcept {
    return OnMessage(view);
  }

  websocket::DeliveryResult OnMessage(
      const websocket::MessageView& view) noexcept {
    return OnMessage(
        view, static_cast<std::int64_t>(websocket::NowNs(clock_source_)));
  }

  websocket::DeliveryResult OnMessage(const websocket::MessageView& view,
                                      std::int64_t local_ns) noexcept {
    if (view.kind != websocket::PayloadKind::kBinary || !view.fin) {
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
      RecordDispatchDrop(dispatch.status);
      return websocket::DeliveryResult::kAccepted;
    }

    switch (dispatch.message_type) {
      case GateSbeMessageType::kBookTicker:
        return OnBookTickerPayload(payload_view, dispatch.header, local_ns);
      default:
        ++stats_.unsupported_sbe_messages;
        return websocket::DeliveryResult::kAccepted;
    }
  }

  [[nodiscard]] const FuturesMarketDataClientStats& stats() const noexcept {
    return stats_;
  }

 private:
  static websocket::DeliveryResult HandleWebSocketMessage(
      void* context, const websocket::MessageView& view) noexcept {
    if (context == nullptr) {
      return websocket::DeliveryResult::kFatal;
    }
    return static_cast<FuturesMarketDataClient*>(context)->OnMessage(view);
  }

  websocket::DeliveryResult OnBookTickerPayload(
      std::string_view payload, const SbeMessageHeader& header,
      std::int64_t local_ns) noexcept {
    const std::string_view symbol = ExtractBookTickerSymbol(payload);
    if (symbol.empty()) [[unlikely]] {
      ++stats_.book_ticker_decode_failures;
      return websocket::DeliveryResult::kAccepted;
    }
    const std::int32_t symbol_id = FindSymbolId(symbol);
    if (symbol_id < 0) [[unlikely]] {
      ++stats_.unknown_symbols;
      return websocket::DeliveryResult::kAccepted;
    }

    BookTicker book_ticker;
    if (!DecodeBookTickerWithHeader(payload, header, local_ns, symbol_id,
                                    &book_ticker)) [[unlikely]] {
      ++stats_.book_ticker_decode_failures;
      return websocket::DeliveryResult::kAccepted;
    }

    consumer_.OnBookTicker(book_ticker);
    return websocket::DeliveryResult::kAccepted;
  }

  std::int32_t FindSymbolId(std::string_view symbol) const noexcept {
    const auto found = symbol_ids_.find(symbol);
    return found == symbol_ids_.end() ? -1 : found->second;
  }

  void BuildSymbolLookup() {
    symbol_ids_.reserve(symbols_.size());
    for (const SymbolBinding& binding : symbols_) {
      if (binding.symbol_id >= 0) {
        symbol_ids_.emplace(binding.symbol, binding.symbol_id);
      }
    }
  }

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

  std::span<const SymbolBinding> symbols_;
  absl::flat_hash_map<std::string_view, std::int32_t> symbol_ids_;
  Consumer& consumer_;
  FuturesMarketDataClientStats stats_{};
  websocket::ClockSource clock_source_;
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_CLIENT_H_
