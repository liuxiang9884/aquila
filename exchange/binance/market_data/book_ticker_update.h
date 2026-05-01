#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_UPDATE_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_UPDATE_H_

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>

#include <fast_float/fast_float.h>

namespace aquila::binance {

inline constexpr size_t kMaxBookTickerSymbolBytes = 32;

enum class BookTickerParseStatus : std::uint8_t {
  kOk = 0,
  kMalformedJson,
  kMissingField,
  kUnsupportedEvent,
  kInvalidNumber,
  kSymbolTooLong,
};

struct BookTickerUpdate {
  BookTickerUpdate() = default;

  BookTickerUpdate(const BookTickerUpdate& other) noexcept {
    *this = other;
  }

  BookTickerUpdate& operator=(const BookTickerUpdate& other) noexcept {
    if (this == &other) {
      return *this;
    }
    update_id = other.update_id;
    event_time_ms = other.event_time_ms;
    transaction_time_ms = other.transaction_time_ms;
    bid_price = other.bid_price;
    bid_volume = other.bid_volume;
    ask_price = other.ask_price;
    ask_volume = other.ask_volume;
    symbol_storage = other.symbol_storage;
    symbol = std::string_view(symbol_storage.data(), other.symbol.size());
    return *this;
  }

  BookTickerUpdate(BookTickerUpdate&& other) noexcept {
    *this = other;
  }

  BookTickerUpdate& operator=(BookTickerUpdate&& other) noexcept {
    return *this = other;
  }

  std::int64_t update_id{0};
  std::int64_t event_time_ms{0};
  std::int64_t transaction_time_ms{0};
  double bid_price{0.0};
  double bid_volume{0.0};
  double ask_price{0.0};
  double ask_volume{0.0};
  std::array<char, kMaxBookTickerSymbolBytes> symbol_storage{};
  std::string_view symbol{};
};

namespace detail {

inline bool ParseDoubleString(std::string_view text, double* output) noexcept {
  if (output == nullptr || text.empty()) {
    return false;
  }
  double parsed = 0.0;
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const fast_float::from_chars_result result =
      fast_float::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  *output = parsed;
  return true;
}

inline bool CopySymbol(std::string_view symbol,
                       BookTickerUpdate* output) noexcept {
  if (output == nullptr || symbol.size() > output->symbol_storage.size()) {
    return false;
  }
  std::memcpy(output->symbol_storage.data(), symbol.data(), symbol.size());
  output->symbol =
      std::string_view(output->symbol_storage.data(), symbol.size());
  return true;
}

}  // namespace detail

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_UPDATE_H_
