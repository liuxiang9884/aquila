#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_SUBSCRIPTION_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_SUBSCRIPTION_H_

#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace aquila::gate {

namespace detail {

inline void AppendQuotedSymbols(fmt::memory_buffer* buffer,
                                std::span<const std::string_view> symbols) {
  for (size_t i = 0; i < symbols.size(); ++i) {
    if (i != 0) {
      fmt::format_to(std::back_inserter(*buffer), ",");
    }
    fmt::format_to(std::back_inserter(*buffer), R"("{}")", symbols[i]);
  }
}

inline std::string BuildFuturesSubscriptionRequest(
    std::string_view channel, std::span<const std::string_view> symbols,
    std::int64_t epoch_seconds, std::string_view event) {
  fmt::memory_buffer buffer;
  fmt::format_to(std::back_inserter(buffer),
                 R"({{"time":{},"channel":"{}","event":"{}","payload":[)",
                 epoch_seconds, channel, event);
  AppendQuotedSymbols(&buffer, symbols);
  fmt::format_to(std::back_inserter(buffer), "]}}");
  return fmt::to_string(buffer);
}

}  // namespace detail

inline std::string BuildFuturesBookTickerSubscribeRequest(
    std::span<const std::string_view> symbols, std::int64_t epoch_seconds) {
  return detail::BuildFuturesSubscriptionRequest("futures.book_ticker", symbols,
                                                 epoch_seconds, "subscribe");
}

inline std::string BuildFuturesBookTickerUnsubscribeRequest(
    std::span<const std::string_view> symbols, std::int64_t epoch_seconds) {
  return detail::BuildFuturesSubscriptionRequest("futures.book_ticker", symbols,
                                                 epoch_seconds, "unsubscribe");
}

inline std::string BuildFuturesTradeSubscribeRequest(
    std::span<const std::string_view> symbols, std::int64_t epoch_seconds) {
  return detail::BuildFuturesSubscriptionRequest("futures.trades", symbols,
                                                 epoch_seconds, "subscribe");
}

inline std::string BuildFuturesTradeUnsubscribeRequest(
    std::span<const std::string_view> symbols, std::int64_t epoch_seconds) {
  return detail::BuildFuturesSubscriptionRequest("futures.trades", symbols,
                                                 epoch_seconds, "unsubscribe");
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_SUBSCRIPTION_H_
