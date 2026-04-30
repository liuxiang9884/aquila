#ifndef AQUILA_EXCHANGE_GATE_TRADING_SUBMIT_RESPONSE_PARSER_H_
#define AQUILA_EXCHANGE_GATE_TRADING_SUBMIT_RESPONSE_PARSER_H_

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include <simdjson.h>

namespace aquila::exchange::gate::trading {

enum class GateSubmitParseStatus : std::uint8_t {
  kOk,
  kInvalidJson,
  kUnexpectedShape,
};

enum class GateSubmitResponseKind : std::uint8_t {
  kUnknown,
  kAck,
  kResult,
  kError,
};

struct GateSubmitResponse {
  GateSubmitParseStatus parse_status{GateSubmitParseStatus::kUnexpectedShape};
  GateSubmitResponseKind kind{GateSubmitResponseKind::kUnknown};
  bool has_ack{false};
  bool ack{false};
  bool channel_is_order_place{false};
  std::uint16_t http_status{0};
  std::uint64_t request_id_hash{0};
  std::uint64_t req_id_hash{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t text_hash{0};
  std::uint64_t error_label_hash{0};
};

inline constexpr std::uint64_t HashGateSubmitString(
    std::string_view value) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char byte : value) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}

namespace detail {

inline bool ParseUintString(std::string_view value,
                            std::uint64_t* output) noexcept {
  if (output == nullptr || value.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  *output = parsed;
  return true;
}

inline GateSubmitResponse InvalidJsonSubmitResponse() noexcept {
  GateSubmitResponse response{};
  response.parse_status = GateSubmitParseStatus::kInvalidJson;
  return response;
}

inline bool ReadSimdjsonString(simdjson::ondemand::value value,
                               std::string_view* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  simdjson::simdjson_result<std::string_view> result = value.get_string();
  std::string_view text{};
  if (std::move(result).get(text) != simdjson::SUCCESS) {
    return false;
  }
  *output = text;
  return true;
}

inline std::uint64_t HashSimdjsonString(
    simdjson::ondemand::value value) noexcept {
  std::string_view text{};
  return ReadSimdjsonString(value, &text) && !text.empty()
             ? HashGateSubmitString(text)
             : 0;
}

inline bool ReadSimdjsonUint64(simdjson::ondemand::value value,
                               std::uint64_t* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  std::uint64_t unsigned_value = 0;
  if (value.get_uint64().get(unsigned_value) == simdjson::SUCCESS) {
    *output = unsigned_value;
    return true;
  }

  std::int64_t signed_value = 0;
  if (value.get_int64().get(signed_value) == simdjson::SUCCESS &&
      signed_value >= 0) {
    *output = static_cast<std::uint64_t>(signed_value);
    return true;
  }

  std::string_view text{};
  return ReadSimdjsonString(value, &text) && ParseUintString(text, output);
}

inline std::uint16_t ReadSimdjsonStatusCode(
    simdjson::ondemand::value value) noexcept {
  std::uint64_t parsed = 0;
  if (!ReadSimdjsonUint64(value, &parsed) || parsed > 999) {
    return 0;
  }
  return static_cast<std::uint16_t>(parsed);
}

inline bool ReadSimdjsonBool(simdjson::ondemand::value value,
                             bool* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  bool parsed = false;
  if (value.get_bool().get(parsed) != simdjson::SUCCESS) {
    return false;
  }
  *output = parsed;
  return true;
}

inline bool FindField(simdjson::ondemand::object object, std::string_view key,
                      simdjson::ondemand::value* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  simdjson::ondemand::value value;
  if (object.find_field_unordered(key).get(value) != simdjson::SUCCESS) {
    return false;
  }
  *output = value;
  return true;
}

inline bool FindObject(simdjson::ondemand::object object, std::string_view key,
                       simdjson::ondemand::object* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  simdjson::ondemand::value value;
  if (!FindField(object, key, &value)) {
    return false;
  }
  simdjson::ondemand::object nested;
  if (value.get_object().get(nested) != simdjson::SUCCESS) {
    return false;
  }
  *output = nested;
  return true;
}

inline GateSubmitResponse ParseSimdjsonDocument(
    simdjson::ondemand::document document) noexcept {
  GateSubmitResponse response{};
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    response.parse_status = GateSubmitParseStatus::kUnexpectedShape;
    return response;
  }

  response.parse_status = GateSubmitParseStatus::kOk;
  simdjson::ondemand::value value;
  if (FindField(root, "request_id", &value)) {
    response.request_id_hash = HashSimdjsonString(value);
  }
  if (FindField(root, "ack", &value)) {
    bool ack = false;
    response.has_ack = ReadSimdjsonBool(value, &ack);
    response.ack = response.has_ack && ack;
  }

  simdjson::ondemand::object header;
  if (FindObject(root, "header", &header)) {
    if (FindField(header, "status", &value)) {
      response.http_status = ReadSimdjsonStatusCode(value);
    }
    if (FindField(header, "channel", &value)) {
      std::string_view channel{};
      response.channel_is_order_place =
          ReadSimdjsonString(value, &channel) &&
          channel == std::string_view("futures.order_place");
    }
  }

  simdjson::ondemand::object data;
  if (!FindObject(root, "data", &data)) {
    return response;
  }

  simdjson::ondemand::object errs;
  if (FindObject(data, "errs", &errs)) {
    response.kind = GateSubmitResponseKind::kError;
    if (FindField(errs, "label", &value)) {
      response.error_label_hash = HashSimdjsonString(value);
    }
    return response;
  }

  simdjson::ondemand::object result;
  if (!FindObject(data, "result", &result)) {
    return response;
  }

  if (response.ack) {
    response.kind = GateSubmitResponseKind::kAck;
    if (FindField(result, "req_id", &value)) {
      response.req_id_hash = HashSimdjsonString(value);
    }
    return response;
  }

  response.kind = GateSubmitResponseKind::kResult;
  if (FindField(result, "id", &value)) {
    (void)ReadSimdjsonUint64(value, &response.exchange_order_id);
  }
  if (FindField(result, "text", &value)) {
    response.text_hash = HashSimdjsonString(value);
  }
  return response;
}

inline GateSubmitResponse ParseSimdjsonAckMinimalDocument(
    simdjson::ondemand::document document) noexcept {
  GateSubmitResponse response{};
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    response.parse_status = GateSubmitParseStatus::kUnexpectedShape;
    return response;
  }

  response.parse_status = GateSubmitParseStatus::kOk;
  simdjson::ondemand::value value;
  if (FindField(root, "request_id", &value)) {
    response.request_id_hash = HashSimdjsonString(value);
  }
  if (FindField(root, "ack", &value)) {
    bool ack = false;
    response.has_ack = ReadSimdjsonBool(value, &ack);
    response.ack = response.has_ack && ack;
    if (response.ack) {
      response.kind = GateSubmitResponseKind::kAck;
    }
  }
  return response;
}

}  // namespace detail

inline GateSubmitResponse ParseGateSubmitResponse(
    std::span<char> padded_payload, size_t payload_size,
    simdjson::ondemand::parser& parser) noexcept {
  if (payload_size == 0 || payload_size > padded_payload.size() ||
      padded_payload.size() - payload_size < simdjson::SIMDJSON_PADDING) {
    return detail::InvalidJsonSubmitResponse();
  }

  simdjson::padded_string_view view(padded_payload.data(), payload_size,
                                    padded_payload.size());
  simdjson::ondemand::document document;
  if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
    return detail::InvalidJsonSubmitResponse();
  }

  return detail::ParseSimdjsonDocument(std::move(document));
}

inline GateSubmitResponse ParseGateSubmitAckMinimal(
    std::span<char> padded_payload, size_t payload_size,
    simdjson::ondemand::parser& parser) noexcept {
  if (payload_size == 0 || payload_size > padded_payload.size() ||
      padded_payload.size() - payload_size < simdjson::SIMDJSON_PADDING) {
    return detail::InvalidJsonSubmitResponse();
  }

  simdjson::padded_string_view view(padded_payload.data(), payload_size,
                                    padded_payload.size());
  simdjson::ondemand::document document;
  if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
    return detail::InvalidJsonSubmitResponse();
  }

  return detail::ParseSimdjsonAckMinimalDocument(std::move(document));
}

inline GateSubmitResponse ParseGateSubmitResponse(
    std::string_view payload) noexcept {
  if (payload.empty()) {
    return detail::InvalidJsonSubmitResponse();
  }

  simdjson::padded_string padded(payload);
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return detail::InvalidJsonSubmitResponse();
  }

  return detail::ParseSimdjsonDocument(std::move(document));
}

inline GateSubmitResponse ParseGateSubmitAckMinimal(
    std::string_view payload) noexcept {
  if (payload.empty()) {
    return detail::InvalidJsonSubmitResponse();
  }

  simdjson::padded_string padded(payload);
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return detail::InvalidJsonSubmitResponse();
  }

  return detail::ParseSimdjsonAckMinimalDocument(std::move(document));
}

inline GateSubmitResponse ParseGateSubmitResponseSimdjson(
    std::span<char> padded_payload, size_t payload_size,
    simdjson::ondemand::parser& parser) noexcept {
  return ParseGateSubmitResponse(padded_payload, payload_size, parser);
}

inline GateSubmitResponse ParseGateSubmitAckMinimalSimdjson(
    std::span<char> padded_payload, size_t payload_size,
    simdjson::ondemand::parser& parser) noexcept {
  return ParseGateSubmitAckMinimal(padded_payload, payload_size, parser);
}

}  // namespace aquila::exchange::gate::trading

#endif  // AQUILA_EXCHANGE_GATE_TRADING_SUBMIT_RESPONSE_PARSER_H_
