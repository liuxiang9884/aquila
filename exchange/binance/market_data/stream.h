#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_STREAM_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_STREAM_H_

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "exchange/binance/market_data/types.h"

namespace aquila::binance {

inline constexpr size_t kMaxFuturesBookTickerStreamsPerConnection = 200;

[[nodiscard]] inline bool IsValidFuturesBookTickerStreamCount(
    size_t stream_count) noexcept {
  return stream_count > 0 &&
         stream_count <= kMaxFuturesBookTickerStreamsPerConnection;
}

[[nodiscard]] inline size_t FuturesMarketDataFeedCount(
    DataSessionFeeds feeds) noexcept {
  return (feeds.book_ticker ? 1U : 0U) + (feeds.trade ? 1U : 0U);
}

[[nodiscard]] inline bool IsValidFuturesMarketDataStreamCount(
    size_t symbol_count, DataSessionFeeds feeds) noexcept {
  const size_t feed_count = FuturesMarketDataFeedCount(feeds);
  return feed_count > 0 && symbol_count > 0 &&
         symbol_count <= kMaxFuturesBookTickerStreamsPerConnection / feed_count;
}

namespace detail {

[[nodiscard]] inline char ToLowerAscii(char value) noexcept {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A'))
                                      : value;
}

inline void AppendLowercase(std::string_view text, std::string* output) {
  for (const char value : text) {
    output->push_back(ToLowerAscii(value));
  }
}

[[nodiscard]] inline std::string_view SymbolText(
    const SymbolBinding& symbol) noexcept {
  return symbol.symbol;
}

[[nodiscard]] inline std::string_view SymbolText(
    std::string_view symbol) noexcept {
  return symbol;
}

template <typename SymbolT>
inline std::string BuildFuturesBookTickerStreamTargetImpl(
    std::span<const SymbolT> symbols) {
  if (!IsValidFuturesBookTickerStreamCount(symbols.size())) {
    return {};
  }

  std::string target;
  target.reserve(11 + symbols.size() * 24);
  target.append("/public/ws/");
  for (size_t i = 0; i < symbols.size(); ++i) {
    if (i != 0) {
      target.push_back('/');
    }
    AppendLowercase(SymbolText(symbols[i]), &target);
    target.append("@bookTicker");
  }
  return target;
}

template <typename SymbolT>
inline std::string BuildFuturesMarketDataStreamTargetImpl(
    std::span<const SymbolT> symbols, DataSessionFeeds feeds) {
  if (!IsValidFuturesMarketDataStreamCount(symbols.size(), feeds)) {
    return {};
  }

  std::string target;
  target.reserve(11 + symbols.size() * FuturesMarketDataFeedCount(feeds) * 24);
  target.append("/public/ws/");
  bool first = true;
  for (const SymbolT& symbol : symbols) {
    if (feeds.book_ticker) {
      if (!first) {
        target.push_back('/');
      }
      first = false;
      AppendLowercase(SymbolText(symbol), &target);
      target.append("@bookTicker");
    }
    if (feeds.trade) {
      if (!first) {
        target.push_back('/');
      }
      first = false;
      AppendLowercase(SymbolText(symbol), &target);
      target.append("@trade");
    }
  }
  return target;
}

}  // namespace detail

[[nodiscard]] inline std::string BuildFuturesBookTickerStreamTarget(
    std::span<const SymbolBinding> symbols) {
  return detail::BuildFuturesBookTickerStreamTargetImpl(symbols);
}

template <size_t N>
[[nodiscard]] inline std::string BuildFuturesBookTickerStreamTarget(
    const std::array<SymbolBinding, N>& symbols) {
  return BuildFuturesBookTickerStreamTarget(
      std::span<const SymbolBinding>(symbols.data(), symbols.size()));
}

[[nodiscard]] inline std::string BuildFuturesBookTickerStreamTarget(
    std::span<const std::string_view> symbols) {
  return detail::BuildFuturesBookTickerStreamTargetImpl(symbols);
}

[[nodiscard]] inline std::string BuildFuturesMarketDataStreamTarget(
    std::span<const SymbolBinding> symbols, DataSessionFeeds feeds) {
  return detail::BuildFuturesMarketDataStreamTargetImpl(symbols, feeds);
}

template <size_t N>
[[nodiscard]] inline std::string BuildFuturesMarketDataStreamTarget(
    const std::array<SymbolBinding, N>& symbols, DataSessionFeeds feeds) {
  return BuildFuturesMarketDataStreamTarget(
      std::span<const SymbolBinding>(symbols.data(), symbols.size()), feeds);
}

[[nodiscard]] inline std::string BuildFuturesMarketDataStreamTarget(
    std::span<const std::string_view> symbols, DataSessionFeeds feeds) {
  return detail::BuildFuturesMarketDataStreamTargetImpl(symbols, feeds);
}

template <size_t N>
[[nodiscard]] inline std::string BuildFuturesMarketDataStreamTarget(
    const std::array<std::string_view, N>& symbols, DataSessionFeeds feeds) {
  return BuildFuturesMarketDataStreamTarget(
      std::span<const std::string_view>(symbols.data(), symbols.size()), feeds);
}

template <size_t N>
[[nodiscard]] inline std::string BuildFuturesBookTickerStreamTarget(
    const std::array<std::string_view, N>& symbols) {
  return BuildFuturesBookTickerStreamTarget(
      std::span<const std::string_view>(symbols.data(), symbols.size()));
}

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_STREAM_H_
