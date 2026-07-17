#ifndef AQUILA_EXCHANGE_BITGET_TRADING_FAST_FILL_PARSER_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_FAST_FILL_PARSER_H_

#include <cassert>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

#include "exchange/common/simdjson_utils.h"
#include <simdjson.h>

namespace aquila::bitget {

enum class FastFillParseStatus : std::uint8_t {
  kOk,
  kInvalidJson,
  kUnexpectedEnvelope,
  kDecodeUnrecoverable,
};

struct FastFillRecord {
  std::string_view category;
  std::string_view symbol;
  std::string_view order_id;
  std::string_view client_oid;
  std::string_view exec_id;
  std::string_view side;
  std::string_view hold_side;
  std::string_view exec_price;
  std::string_view exec_quantity;
  std::string_view trade_scope;
  std::uint64_t exchange_message_time_ms{0};
  std::uint64_t exec_time_ms{0};
  std::uint64_t updated_time_ms{0};
};

struct FastFillParserStats {
  std::uint64_t messages_seen{0};
  std::uint64_t messages_parsed{0};
  std::uint64_t records_emitted{0};
  std::uint64_t invalid_json_count{0};
  std::uint64_t unexpected_envelope_count{0};
  std::uint64_t validation_errors{0};
};

struct FastFillParseResult {
  FastFillParseStatus status{FastFillParseStatus::kUnexpectedEnvelope};
  std::uint32_t records_emitted{0};
};

namespace fast_fill_detail {

using ::aquila::exchange::detail::FindSimdjsonField;
using ::aquila::exchange::detail::FindSimdjsonObject;
using ::aquila::exchange::detail::ReadSimdjsonString;

[[nodiscard]] inline bool ParseUint64Text(std::string_view text,
                                          std::uint64_t* output) noexcept {
  assert(output != nullptr);
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const char* const end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return false;
  }
  *output = value;
  return true;
}

[[nodiscard]] inline bool ReadUint64(simdjson::ondemand::value value,
                                     std::uint64_t* output) noexcept {
  std::string_view text;
  if (ReadSimdjsonString(value, &text)) {
    return ParseUint64Text(text, output);
  }
  std::uint64_t number = 0;
  if (value.get_uint64().get(number) != simdjson::SUCCESS) {
    return false;
  }
  *output = number;
  return true;
}

[[nodiscard]] inline bool ReadStringField(simdjson::ondemand::object object,
                                          std::string_view name,
                                          std::string_view* output) noexcept {
  simdjson::ondemand::value value;
  return FindSimdjsonField(object, name, &value) &&
         ReadSimdjsonString(value, output);
}

[[nodiscard]] inline bool ReadUint64Field(simdjson::ondemand::object object,
                                          std::string_view name,
                                          std::uint64_t* output) noexcept {
  simdjson::ondemand::value value;
  return FindSimdjsonField(object, name, &value) && ReadUint64(value, output);
}

template <typename RecordSink>
[[nodiscard]] inline FastFillParseResult ParseDocument(
    simdjson::ondemand::document document, FastFillParserStats& stats,
    RecordSink& record_sink) noexcept {
  FastFillParseResult result{};
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    ++stats.unexpected_envelope_count;
    return result;
  }

  std::string_view action;
  simdjson::ondemand::value action_value;
  simdjson::ondemand::object arg;
  std::string_view inst_type;
  std::string_view topic;
  std::uint64_t exchange_message_time_ms = 0;
  simdjson::ondemand::object data;
  if (!FindSimdjsonField(root, "action", &action_value) ||
      !ReadSimdjsonString(action_value, &action) || action != "update" ||
      !FindSimdjsonObject(root, "arg", &arg) ||
      !ReadStringField(arg, "instType", &inst_type) || inst_type != "UTA" ||
      !ReadStringField(arg, "topic", &topic) || topic != "fast-fill" ||
      !ReadUint64Field(root, "ts", &exchange_message_time_ms) ||
      !FindSimdjsonObject(root, "data", &data)) {
    ++stats.unexpected_envelope_count;
    return result;
  }

  FastFillRecord record{
      .exchange_message_time_ms = exchange_message_time_ms,
  };
  if (!ReadStringField(data, "category", &record.category) ||
      !ReadStringField(data, "symbol", &record.symbol) ||
      !ReadStringField(data, "orderId", &record.order_id) ||
      !ReadStringField(data, "clientOid", &record.client_oid) ||
      !ReadStringField(data, "execId", &record.exec_id) ||
      !ReadStringField(data, "side", &record.side) ||
      !ReadStringField(data, "holdSide", &record.hold_side) ||
      !ReadStringField(data, "execPrice", &record.exec_price) ||
      !ReadStringField(data, "execQty", &record.exec_quantity) ||
      !ReadStringField(data, "tradeScope", &record.trade_scope) ||
      !ReadUint64Field(data, "execTime", &record.exec_time_ms) ||
      !ReadUint64Field(data, "updatedTime", &record.updated_time_ms) ||
      record.category.empty() || record.symbol.empty() ||
      record.order_id.empty() || record.client_oid.empty() ||
      record.exec_id.empty() || record.side.empty() ||
      record.exec_price.empty() || record.exec_quantity.empty() ||
      record.trade_scope.empty() || record.exchange_message_time_ms == 0 ||
      record.exec_time_ms == 0 || record.updated_time_ms == 0) {
    ++stats.validation_errors;
    result.status = FastFillParseStatus::kDecodeUnrecoverable;
    return result;
  }

  record_sink(record);
  ++stats.records_emitted;
  ++stats.messages_parsed;
  ++result.records_emitted;
  result.status = FastFillParseStatus::kOk;
  return result;
}

[[nodiscard]] inline FastFillParseResult ClassifyDeferredJsonError(
    std::string_view payload, FastFillParseResult result,
    FastFillParserStats& stats) noexcept {
  if (result.status == FastFillParseStatus::kOk) {
    return result;
  }
  simdjson::padded_string padded(payload);
  simdjson::dom::parser validator;
  simdjson::dom::element element;
  if (validator.parse(padded).get(element) == simdjson::SUCCESS) {
    return result;
  }
  if (result.status == FastFillParseStatus::kUnexpectedEnvelope &&
      stats.unexpected_envelope_count != 0) {
    --stats.unexpected_envelope_count;
  } else if (result.status == FastFillParseStatus::kDecodeUnrecoverable &&
             stats.validation_errors != 0) {
    --stats.validation_errors;
  }
  ++stats.invalid_json_count;
  result.status = FastFillParseStatus::kInvalidJson;
  return result;
}

}  // namespace fast_fill_detail

template <typename RecordSink>
[[nodiscard]] inline FastFillParseResult ParseBitgetFastFillMessage(
    std::string_view payload, std::uint32_t readable_tail_bytes,
    simdjson::ondemand::parser& parser, FastFillParserStats& stats,
    RecordSink&& record_sink) noexcept {
  ++stats.messages_seen;
  if (payload.empty()) {
    ++stats.invalid_json_count;
    return {.status = FastFillParseStatus::kInvalidJson};
  }

  simdjson::ondemand::document document;
  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    const simdjson::padded_string_view view(
        payload.data(), payload.size(), payload.size() + readable_tail_bytes);
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      ++stats.invalid_json_count;
      return {.status = FastFillParseStatus::kInvalidJson};
    }
    return fast_fill_detail::ClassifyDeferredJsonError(
        payload,
        fast_fill_detail::ParseDocument(std::move(document), stats,
                                        record_sink),
        stats);
  }

  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    ++stats.invalid_json_count;
    return {.status = FastFillParseStatus::kInvalidJson};
  }
  return fast_fill_detail::ClassifyDeferredJsonError(
      payload,
      fast_fill_detail::ParseDocument(std::move(document), stats, record_sink),
      stats);
}

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_FAST_FILL_PARSER_H_
