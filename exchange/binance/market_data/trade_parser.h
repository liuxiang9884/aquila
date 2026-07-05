#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_TRADE_PARSER_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_TRADE_PARSER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>
#include <utility>

#include <fast_float/fast_float.h>

#include <simdjson.h>

namespace aquila::binance {

inline constexpr size_t kMaxTradeSymbolBytes = 32;

enum class TradeParseStatus : std::uint8_t {
  kOk = 0,
  kMalformedJson,
};

struct TradeUpdate {
  TradeUpdate() = default;

  TradeUpdate(const TradeUpdate& other) noexcept {
    *this = other;
  }

  TradeUpdate& operator=(const TradeUpdate& other) noexcept {
    if (this == &other) {
      return *this;
    }
    trade_id = other.trade_id;
    event_time_ms = other.event_time_ms;
    trade_time_ms = other.trade_time_ms;
    price = other.price;
    volume = other.volume;
    buyer_is_maker = other.buyer_is_maker;
    symbol_storage = other.symbol_storage;
    symbol = std::string_view(symbol_storage.data(), other.symbol.size());
    return *this;
  }

  TradeUpdate(TradeUpdate&& other) noexcept {
    *this = other;
  }

  TradeUpdate& operator=(TradeUpdate&& other) noexcept {
    return *this = other;
  }

  std::int64_t trade_id;
  std::int64_t event_time_ms;
  std::int64_t trade_time_ms;
  double price;
  double volume;
  bool buyer_is_maker;
  std::array<char, kMaxTradeSymbolBytes> symbol_storage;
  std::string_view symbol;
};

namespace detail {

inline bool ReadTradeStringField(simdjson::ondemand::object& object,
                                 std::string_view key,
                                 std::string_view* output) noexcept {
  simdjson::simdjson_result<simdjson::ondemand::value> result =
      object.find_field_unordered(key);
  if (result.error() != simdjson::SUCCESS) {
    return false;
  }
  simdjson::simdjson_result<std::string_view> text =
      result.value_unsafe().get_string();
  if (text.error() != simdjson::SUCCESS) {
    return false;
  }
  *output = text.value_unsafe();
  return true;
}

inline bool ReadTradeInt64Field(simdjson::ondemand::object& object,
                                std::string_view key,
                                std::int64_t* output) noexcept {
  simdjson::simdjson_result<simdjson::ondemand::value> result =
      object.find_field_unordered(key);
  if (result.error() != simdjson::SUCCESS) {
    return false;
  }
  simdjson::simdjson_result<std::int64_t> parsed =
      result.value_unsafe().get_int64();
  if (parsed.error() != simdjson::SUCCESS) {
    return false;
  }
  *output = parsed.value_unsafe();
  return true;
}

inline bool ReadTradeBoolField(simdjson::ondemand::object& object,
                               std::string_view key, bool* output) noexcept {
  simdjson::simdjson_result<simdjson::ondemand::value> result =
      object.find_field_unordered(key);
  if (result.error() != simdjson::SUCCESS) {
    return false;
  }
  simdjson::simdjson_result<bool> parsed = result.value_unsafe().get_bool();
  if (parsed.error() != simdjson::SUCCESS) {
    return false;
  }
  *output = parsed.value_unsafe();
  return true;
}

inline bool ReadTradeDoubleField(simdjson::ondemand::object& object,
                                 std::string_view key,
                                 double* output) noexcept {
  std::string_view text;
  if (!ReadTradeStringField(object, key, &text) || text.empty()) {
    return false;
  }
  const auto parsed =
      fast_float::from_chars(text.data(), text.data() + text.size(), *output);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

inline bool CopyTradeSymbolToStorage(std::string_view symbol,
                                     TradeUpdate& output) noexcept {
  if (symbol.empty() || symbol.size() > output.symbol_storage.size()) {
    return false;
  }
  std::memcpy(output.symbol_storage.data(), symbol.data(), symbol.size());
  output.symbol = std::string_view(output.symbol_storage.data(), symbol.size());
  return true;
}

inline TradeParseStatus ParseTradeObject(simdjson::ondemand::object root,
                                         TradeUpdate& output) noexcept {
  std::int64_t event_time_ms = 0;
  std::int64_t trade_time_ms = 0;
  std::string_view symbol;
  std::int64_t trade_id = 0;
  double price = 0.0;
  double volume = 0.0;
  bool buyer_is_maker = false;
  if (!ReadTradeInt64Field(root, "E", &event_time_ms) ||
      !ReadTradeInt64Field(root, "T", &trade_time_ms) ||
      !ReadTradeStringField(root, "s", &symbol) ||
      !ReadTradeInt64Field(root, "t", &trade_id) ||
      !ReadTradeDoubleField(root, "p", &price) ||
      !ReadTradeDoubleField(root, "q", &volume) ||
      !ReadTradeBoolField(root, "m", &buyer_is_maker)) {
    return TradeParseStatus::kMalformedJson;
  }

  output.trade_id = trade_id;
  output.event_time_ms = event_time_ms;
  output.trade_time_ms = trade_time_ms;
  output.price = price;
  output.volume = volume;
  output.buyer_is_maker = buyer_is_maker;
  if (!CopyTradeSymbolToStorage(symbol, output)) {
    return TradeParseStatus::kMalformedJson;
  }
  return TradeParseStatus::kOk;
}

inline TradeParseStatus ParseTradeDocument(
    simdjson::ondemand::document document, TradeUpdate& output) noexcept {
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    return TradeParseStatus::kMalformedJson;
  }
  return ParseTradeObject(root, output);
}

}  // namespace detail

inline TradeParseStatus ParseTrade(std::string_view payload,
                                   std::uint32_t readable_tail_bytes,
                                   simdjson::ondemand::parser& parser,
                                   TradeUpdate& output) noexcept {
  if (payload.empty()) {
    return TradeParseStatus::kMalformedJson;
  }

  simdjson::ondemand::document document;
  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    simdjson::padded_string_view view(payload.data(), payload.size(),
                                      payload.size() + readable_tail_bytes);
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      return TradeParseStatus::kMalformedJson;
    }
    return detail::ParseTradeDocument(std::move(document), output);
  }

  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return TradeParseStatus::kMalformedJson;
  }
  return detail::ParseTradeDocument(std::move(document), output);
}

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_TRADE_PARSER_H_
