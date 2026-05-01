#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include <fast_float/fast_float.h>

#include "exchange/common/simdjson_utils.h"
#include <simdjson.h>

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

inline BookTickerParseStatus ParseBookTickerObject(
    simdjson::ondemand::object root, BookTickerUpdate* output) noexcept {
  if (output == nullptr) {
    return BookTickerParseStatus::kMissingField;
  }

  namespace json = aquila::exchange::detail;
  simdjson::ondemand::value value;

  std::string_view event;
  if (!json::FindSimdjsonField(root, "e", &value) ||
      !json::ReadSimdjsonString(value, &event)) {
    return BookTickerParseStatus::kMissingField;
  }
  if (event != "bookTicker") {
    return BookTickerParseStatus::kUnsupportedEvent;
  }

  std::int64_t update_id = 0;
  if (!json::FindSimdjsonField(root, "u", &value) ||
      !json::ReadSimdjsonInt64(value, &update_id)) {
    return BookTickerParseStatus::kMissingField;
  }

  std::int64_t event_time_ms = 0;
  if (!json::FindSimdjsonField(root, "E", &value) ||
      !json::ReadSimdjsonInt64(value, &event_time_ms)) {
    return BookTickerParseStatus::kMissingField;
  }

  std::int64_t transaction_time_ms = 0;
  if (!json::FindSimdjsonField(root, "T", &value) ||
      !json::ReadSimdjsonInt64(value, &transaction_time_ms)) {
    return BookTickerParseStatus::kMissingField;
  }

  std::string_view symbol;
  if (!json::FindSimdjsonField(root, "s", &value) ||
      !json::ReadSimdjsonString(value, &symbol)) {
    return BookTickerParseStatus::kMissingField;
  }

  double bid_price = 0.0;
  std::string_view number;
  if (!json::FindSimdjsonField(root, "b", &value) ||
      !json::ReadSimdjsonString(value, &number) ||
      !ParseDoubleString(number, &bid_price)) {
    return BookTickerParseStatus::kInvalidNumber;
  }

  double bid_volume = 0.0;
  if (!json::FindSimdjsonField(root, "B", &value) ||
      !json::ReadSimdjsonString(value, &number) ||
      !ParseDoubleString(number, &bid_volume)) {
    return BookTickerParseStatus::kInvalidNumber;
  }

  double ask_price = 0.0;
  if (!json::FindSimdjsonField(root, "a", &value) ||
      !json::ReadSimdjsonString(value, &number) ||
      !ParseDoubleString(number, &ask_price)) {
    return BookTickerParseStatus::kInvalidNumber;
  }

  double ask_volume = 0.0;
  if (!json::FindSimdjsonField(root, "A", &value) ||
      !json::ReadSimdjsonString(value, &number) ||
      !ParseDoubleString(number, &ask_volume)) {
    return BookTickerParseStatus::kInvalidNumber;
  }

  output->update_id = update_id;
  output->event_time_ms = event_time_ms;
  output->transaction_time_ms = transaction_time_ms;
  output->bid_price = bid_price;
  output->bid_volume = bid_volume;
  output->ask_price = ask_price;
  output->ask_volume = ask_volume;
  if (!CopySymbol(symbol, output)) {
    return BookTickerParseStatus::kSymbolTooLong;
  }
  return BookTickerParseStatus::kOk;
}

inline BookTickerParseStatus ParseBookTickerDocument(
    simdjson::ondemand::document document, BookTickerUpdate* output) noexcept {
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
    return detail::ParseBookTickerDocument(std::move(document), &output);
  }

  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return BookTickerParseStatus::kMalformedJson;
  }
  return detail::ParseBookTickerDocument(std::move(document), &output);
}

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_
