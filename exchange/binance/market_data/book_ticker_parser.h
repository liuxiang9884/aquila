#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "core/utils/numeric.h"
#include <simdjson.h>

namespace aquila::binance {

inline constexpr size_t kMaxBookTickerSymbolBytes = 32;

enum class BookTickerParseStatus : std::uint8_t {
  kOk = 0,
  kMalformedJson,
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

  std::int64_t update_id;
  std::int64_t event_time_ms;
  std::int64_t transaction_time_ms;
  double bid_price;
  double bid_volume;
  double ask_price;
  double ask_volume;
  std::array<char, kMaxBookTickerSymbolBytes> symbol_storage;
  std::string_view symbol;
};

namespace detail {

inline simdjson::ondemand::value Field(simdjson::ondemand::object& object,
                                       std::string_view key) noexcept {
  simdjson::simdjson_result<simdjson::ondemand::value> result =
      object.find_field_unordered(key);
  assert(result.error() == simdjson::SUCCESS);
  return result.value_unsafe();
}

inline std::string_view StringValue(simdjson::ondemand::value value) noexcept {
  simdjson::simdjson_result<std::string_view> result = value.get_string();
  assert(result.error() == simdjson::SUCCESS);
  return result.value_unsafe();
}

inline std::int64_t Int64Value(simdjson::ondemand::value value) noexcept {
  simdjson::simdjson_result<std::int64_t> result = value.get_int64();
  assert(result.error() == simdjson::SUCCESS);
  return result.value_unsafe();
}

inline void CopySymbolToStorage(std::string_view symbol,
                                BookTickerUpdate& output) noexcept {
  assert(symbol.size() <= output.symbol_storage.size());
  std::memcpy(output.symbol_storage.data(), symbol.data(), symbol.size());
  output.symbol = std::string_view(output.symbol_storage.data(), symbol.size());
}

inline BookTickerParseStatus ParseBookTickerObject(
    simdjson::ondemand::object root, BookTickerUpdate& output) noexcept {
  const std::int64_t update_id = Int64Value(Field(root, "u"));
  const std::int64_t event_time_ms = Int64Value(Field(root, "E"));
  const std::int64_t transaction_time_ms = Int64Value(Field(root, "T"));
  const std::string_view symbol = StringValue(Field(root, "s"));
  const double bid_price = ToDouble(StringValue(Field(root, "b")));
  const double bid_volume = ToDouble(StringValue(Field(root, "B")));
  const double ask_price = ToDouble(StringValue(Field(root, "a")));
  const double ask_volume = ToDouble(StringValue(Field(root, "A")));

  output.update_id = update_id;
  output.event_time_ms = event_time_ms;
  output.transaction_time_ms = transaction_time_ms;
  output.bid_price = bid_price;
  output.bid_volume = bid_volume;
  output.ask_price = ask_price;
  output.ask_volume = ask_volume;
  CopySymbolToStorage(symbol, output);
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
