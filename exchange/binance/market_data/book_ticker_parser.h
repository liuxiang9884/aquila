#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_

#include <cstdint>
#include <string_view>
#include <utility>

#include "exchange/binance/market_data/book_ticker_update.h"
#include "exchange/common/simdjson_utils.h"
#include <simdjson.h>

namespace aquila::binance {

namespace detail {

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

class SimdjsonBookTickerParser {
 public:
  BookTickerParseStatus Parse(std::string_view payload,
                              std::uint32_t readable_tail_bytes,
                              BookTickerUpdate& output) noexcept {
    return ParseBookTicker(payload, readable_tail_bytes, parser_, output);
  }

 private:
  simdjson::ondemand::parser parser_;
};

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_PARSER_H_
