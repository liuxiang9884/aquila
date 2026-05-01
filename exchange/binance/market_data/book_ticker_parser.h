#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_

#include <array>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include <fast_float/fast_float.h>

#include <simdjson.h>

namespace aquila::binance {

inline constexpr size_t kMaxBookTickerSymbolBytes = 32;

enum class BookTickerParseStatus : std::uint8_t {
  kOk = 0,
  kMalformedJson,
};

struct BookTickerUpdate {
  // Successful parses fully assign every field; do not read after parse
  // failure.
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

  std::int64_t update_id;
  std::int64_t event_time_ms;
  double bid_price;
  double bid_volume;
  double ask_price;
  double ask_volume;
  std::array<char, kMaxBookTickerSymbolBytes> symbol_storage;
  std::string_view symbol;
};

namespace detail {

inline simdjson::ondemand::value TrustedField(
    simdjson::ondemand::object& object, std::string_view key) noexcept {
  simdjson::simdjson_result<simdjson::ondemand::value> result =
      object.find_field_unordered(key);
  assert(result.error() == simdjson::SUCCESS);
  return result.value_unsafe();
}

inline std::string_view TrustedString(
    simdjson::ondemand::value value) noexcept {
  simdjson::simdjson_result<std::string_view> result = value.get_string();
  assert(result.error() == simdjson::SUCCESS);
  return result.value_unsafe();
}

inline std::int64_t TrustedInt64(simdjson::ondemand::value value) noexcept {
  simdjson::simdjson_result<std::int64_t> result = value.get_int64();
  assert(result.error() == simdjson::SUCCESS);
  return result.value_unsafe();
}

inline double ParseTrustedDoubleString(std::string_view text) noexcept {
  assert(!text.empty());
  double parsed = 0.0;
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const fast_float::from_chars_result result =
      fast_float::from_chars(begin, end, parsed);
  assert(result.ec == std::errc{} && result.ptr == end);
  return parsed;
}

inline void CopyTrustedSymbol(std::string_view symbol,
                              BookTickerUpdate& output) noexcept {
  assert(symbol.size() <= output.symbol_storage.size());
  std::memcpy(output.symbol_storage.data(), symbol.data(), symbol.size());
  output.symbol = std::string_view(output.symbol_storage.data(), symbol.size());
}

inline BookTickerParseStatus ParseBookTickerObject(
    simdjson::ondemand::object root, BookTickerUpdate& output) noexcept {
  const std::int64_t update_id = TrustedInt64(TrustedField(root, "u"));
  const std::int64_t event_time_ms = TrustedInt64(TrustedField(root, "E"));
  const std::string_view symbol = TrustedString(TrustedField(root, "s"));
  const double bid_price =
      ParseTrustedDoubleString(TrustedString(TrustedField(root, "b")));
  const double bid_volume =
      ParseTrustedDoubleString(TrustedString(TrustedField(root, "B")));
  const double ask_price =
      ParseTrustedDoubleString(TrustedString(TrustedField(root, "a")));
  const double ask_volume =
      ParseTrustedDoubleString(TrustedString(TrustedField(root, "A")));

  output.update_id = update_id;
  output.event_time_ms = event_time_ms;
  output.bid_price = bid_price;
  output.bid_volume = bid_volume;
  output.ask_price = ask_price;
  output.ask_volume = ask_volume;
  CopyTrustedSymbol(symbol, output);
  return BookTickerParseStatus::kOk;
}

inline BookTickerParseStatus ParseBookTickerDocument(
    simdjson::ondemand::document document, BookTickerUpdate& output) noexcept {
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    return BookTickerParseStatus::kMalformedJson;
  }
  return ParseBookTickerObject(root, output);
}

}  // namespace detail

inline BookTickerParseStatus ParseBookTicker(
    std::string_view payload, std::uint32_t readable_tail_bytes,
    simdjson::ondemand::parser& parser, BookTickerUpdate& output) noexcept {
  if (payload.empty()) {
    return BookTickerParseStatus::kMalformedJson;
  }

  simdjson::ondemand::document document;
  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    simdjson::padded_string_view view(payload.data(), payload.size(),
                                      payload.size() + readable_tail_bytes);
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      return BookTickerParseStatus::kMalformedJson;
    }
    return detail::ParseBookTickerDocument(std::move(document), output);
  }

  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return BookTickerParseStatus::kMalformedJson;
  }
  return detail::ParseBookTickerDocument(std::move(document), output);
}

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_
