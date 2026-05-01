#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_YYJSON_PARSER_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_YYJSON_PARSER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "exchange/binance/market_data/book_ticker_update.h"
#include <yyjson.h>

namespace aquila::binance {

inline constexpr size_t kYyjsonBookTickerReadPoolBytes = 4096;

namespace detail {

class YyjsonDoc {
 public:
  explicit YyjsonDoc(yyjson_doc* doc) noexcept : doc_(doc) {}
  YyjsonDoc(const YyjsonDoc&) = delete;
  YyjsonDoc& operator=(const YyjsonDoc&) = delete;
  ~YyjsonDoc() {
    if (doc_ != nullptr) {
      yyjson_doc_free(doc_);
    }
  }

  [[nodiscard]] yyjson_doc* get() const noexcept {
    return doc_;
  }

 private:
  yyjson_doc* doc_{nullptr};
};

inline bool ReadYyjsonString(yyjson_val* value,
                             std::string_view* output) noexcept {
  if (output == nullptr || !yyjson_is_str(value)) {
    return false;
  }
  const char* const text = yyjson_get_str(value);
  if (text == nullptr) {
    return false;
  }
  *output = std::string_view(text, yyjson_get_len(value));
  return true;
}

inline bool ReadYyjsonInt64(yyjson_val* value, std::int64_t* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  if (yyjson_is_uint(value)) {
    const std::uint64_t parsed = yyjson_get_uint(value);
    if (parsed >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    *output = static_cast<std::int64_t>(parsed);
    return true;
  }
  if (yyjson_is_sint(value)) {
    *output = yyjson_get_sint(value);
    return true;
  }
  return false;
}

inline BookTickerParseStatus ParseYyjsonBookTickerDocument(
    yyjson_doc* doc, BookTickerUpdate* output) noexcept {
  if (doc == nullptr) {
    return BookTickerParseStatus::kMalformedJson;
  }
  if (output == nullptr) {
    return BookTickerParseStatus::kMissingField;
  }

  yyjson_val* root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    return BookTickerParseStatus::kMalformedJson;
  }

  std::string_view event;
  if (!ReadYyjsonString(yyjson_obj_get(root, "e"), &event)) {
    return BookTickerParseStatus::kMissingField;
  }
  if (event != "bookTicker") {
    return BookTickerParseStatus::kUnsupportedEvent;
  }

  std::int64_t update_id = 0;
  if (!ReadYyjsonInt64(yyjson_obj_get(root, "u"), &update_id)) {
    return BookTickerParseStatus::kMissingField;
  }

  std::int64_t event_time_ms = 0;
  if (!ReadYyjsonInt64(yyjson_obj_get(root, "E"), &event_time_ms)) {
    return BookTickerParseStatus::kMissingField;
  }

  std::int64_t transaction_time_ms = 0;
  if (!ReadYyjsonInt64(yyjson_obj_get(root, "T"), &transaction_time_ms)) {
    return BookTickerParseStatus::kMissingField;
  }

  std::string_view symbol;
  if (!ReadYyjsonString(yyjson_obj_get(root, "s"), &symbol)) {
    return BookTickerParseStatus::kMissingField;
  }

  double bid_price = 0.0;
  std::string_view number;
  if (!ReadYyjsonString(yyjson_obj_get(root, "b"), &number) ||
      !ParseDoubleString(number, &bid_price)) {
    return BookTickerParseStatus::kInvalidNumber;
  }

  double bid_volume = 0.0;
  if (!ReadYyjsonString(yyjson_obj_get(root, "B"), &number) ||
      !ParseDoubleString(number, &bid_volume)) {
    return BookTickerParseStatus::kInvalidNumber;
  }

  double ask_price = 0.0;
  if (!ReadYyjsonString(yyjson_obj_get(root, "a"), &number) ||
      !ParseDoubleString(number, &ask_price)) {
    return BookTickerParseStatus::kInvalidNumber;
  }

  double ask_volume = 0.0;
  if (!ReadYyjsonString(yyjson_obj_get(root, "A"), &number) ||
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

}  // namespace detail

template <size_t ReadPoolBytes = kYyjsonBookTickerReadPoolBytes>
class BasicYyjsonBookTickerParser {
 public:
  BookTickerParseStatus Parse(std::string_view payload,
                              std::uint32_t readable_tail_bytes,
                              BookTickerUpdate& output) noexcept {
    (void)readable_tail_bytes;
    if (payload.empty()) {
      return BookTickerParseStatus::kMalformedJson;
    }

    return ParseWithFlags(const_cast<char*>(payload.data()), payload.size(),
                          YYJSON_READ_NOFLAG, output);
  }

  BookTickerParseStatus ParseInsitu(std::span<char> padded_payload,
                                    size_t payload_size,
                                    BookTickerUpdate& output) noexcept {
    if (payload_size == 0 || payload_size > padded_payload.size() ||
        padded_payload.size() - payload_size < YYJSON_PADDING_SIZE) {
      return BookTickerParseStatus::kMalformedJson;
    }

    return ParseWithFlags(padded_payload.data(), payload_size,
                          YYJSON_READ_INSITU, output);
  }

 private:
  BookTickerParseStatus ParseWithFlags(char* payload, size_t payload_size,
                                       yyjson_read_flag flags,
                                       BookTickerUpdate& output) noexcept {
    yyjson_alc allocator{};
    if (!yyjson_alc_pool_init(&allocator, read_pool_.data(),
                              read_pool_.size())) {
      return BookTickerParseStatus::kMalformedJson;
    }

    yyjson_read_err error{};
    detail::YyjsonDoc doc(
        yyjson_read_opts(payload, payload_size, flags, &allocator, &error));
    if (doc.get() == nullptr) {
      return BookTickerParseStatus::kMalformedJson;
    }
    return detail::ParseYyjsonBookTickerDocument(doc.get(), &output);
  }

  alignas(std::max_align_t) std::array<char, ReadPoolBytes> read_pool_{};
};

using YyjsonBookTickerParser = BasicYyjsonBookTickerParser<>;

template <size_t ReadPoolBytes = kYyjsonBookTickerReadPoolBytes>
class BasicYyjsonInsituBookTickerParser {
 public:
  BookTickerParseStatus Parse(std::string_view payload,
                              std::uint32_t readable_tail_bytes,
                              BookTickerUpdate& output) noexcept {
    if (payload.empty()) {
      return BookTickerParseStatus::kMalformedJson;
    }
    if (readable_tail_bytes >= YYJSON_PADDING_SIZE) {
      // This parser policy is only safe for mutable receive buffers. It is
      // intended for FrameCodec-backed views whose underlying ring storage is
      // writable even though MessageView exposes a const payload span.
      return parser_.ParseInsitu(
          std::span<char>(const_cast<char*>(payload.data()),
                          payload.size() + readable_tail_bytes),
          payload.size(), output);
    }
    return parser_.Parse(payload, readable_tail_bytes, output);
  }

 private:
  BasicYyjsonBookTickerParser<ReadPoolBytes> parser_;
};

using YyjsonInsituBookTickerParser = BasicYyjsonInsituBookTickerParser<>;

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_BOOK_TICKER_YYJSON_PARSER_H_
