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

template <size_t N>
[[nodiscard]] inline std::string BuildFuturesBookTickerStreamTarget(
    const std::array<std::string_view, N>& symbols) {
  return BuildFuturesBookTickerStreamTarget(
      std::span<const std::string_view>(symbols.data(), symbols.size()));
}

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_STREAM_H_
