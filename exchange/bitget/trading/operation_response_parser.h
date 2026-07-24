#ifndef AQUILA_EXCHANGE_BITGET_TRADING_OPERATION_RESPONSE_PARSER_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_OPERATION_RESPONSE_PARSER_H_

#include <cassert>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

#include "exchange/bitget/trading/order_codecs.h"
#include "exchange/common/simdjson_utils.h"
#include <simdjson.h>

namespace aquila::bitget {

enum class OperationParseStatus : std::uint8_t {
  kOk,
  kInvalidJson,
  kUnexpectedShape,
};

enum class OperationTopic : std::uint8_t {
  kUnknown,
  kPlaceOrder,
  kCancelOrder,
};

enum class OperationResponseKind : std::uint8_t {
  kUnknown,
  kLoginAccepted,
  kLoginRejected,
  kAck,
  kRejected,
  kCancelRejected,
  kUnknownResult,
};

struct OperationResponse {
  OperationParseStatus parse_status{OperationParseStatus::kUnexpectedShape};
  OperationResponseKind kind{OperationResponseKind::kUnknown};
  OperationTopic topic{OperationTopic::kUnknown};
  DecodedRequestId request_id{};
  ParsedClientOid client_oid{};
  std::uint64_t exchange_order_id{0};
  std::uint64_t connection_id_hash{0};
  std::uint64_t error_message_hash{0};
  std::uint32_t error_code{0};
  std::int64_t creation_time_ms{0};
  std::int64_t exchange_ns{0};
};

[[nodiscard]] inline constexpr std::uint64_t HashBitgetOperationString(
    std::string_view value) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char byte : value) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}

namespace detail {

using ::aquila::exchange::detail::FindSimdjsonField;
using ::aquila::exchange::detail::ReadSimdjsonString;

[[nodiscard]] inline bool ParseUint64Text(std::string_view text,
                                          std::uint64_t* output) noexcept {
  assert(output != nullptr);
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const char* const first = text.data();
  const char* const last = text.data() + text.size();
  const auto result = std::from_chars(first, last, value);
  if (result.ec != std::errc{} || result.ptr != last) {
    return false;
  }
  *output = value;
  return true;
}

[[nodiscard]] inline bool ReadUint64(simdjson::ondemand::value value,
                                     std::uint64_t* output) noexcept {
  assert(output != nullptr);
  std::string_view text{};
  if (ReadSimdjsonString(value, &text)) {
    return ParseUint64Text(text, output);
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
  return false;
}

[[nodiscard]] inline OperationTopic ParseTopic(std::string_view topic) {
  if (topic == "place-order") {
    return OperationTopic::kPlaceOrder;
  }
  if (topic == "cancel-order") {
    return OperationTopic::kCancelOrder;
  }
  return OperationTopic::kUnknown;
}

[[nodiscard]] inline bool TopicMatchesRequest(
    OperationTopic topic, OrderRequestType request_type) noexcept {
  return (topic == OperationTopic::kPlaceOrder &&
          request_type == OrderRequestType::kPlaceOrder) ||
         (topic == OperationTopic::kCancelOrder &&
          request_type == OrderRequestType::kCancelOrder);
}

[[nodiscard]] inline bool IsDefiniteBusinessReject(
    std::uint32_t code) noexcept {
  if (code == 30004 || (code >= 30011 && code <= 30015)) {
    return true;
  }
  if (code >= 22001 && code <= 22099) {
    return true;
  }
  if (code >= 25100 && code <= 25299) {
    return true;
  }
  switch (code) {
    case 40000:
    case 40022:
    case 40023:
    case 40025:
    case 40026:
    case 40031:
    case 40035:
    case 40103:
    case 40710:
    case 40715:
    case 40760:
    case 40761:
    case 40762:
    case 40763:
    case 40769:
    case 40774:
    case 40775:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] inline bool IsDocumentedTopiclessPlaceAmbiguity(
    std::uint32_t code, OrderRequestType request_type) noexcept {
  return request_type == OrderRequestType::kPlaceOrder &&
         (code == 40010 || code == 40725 || code == 45001);
}

[[nodiscard]] inline bool ScaleMillisecondsToNanoseconds(
    std::uint64_t milliseconds, std::int64_t* output) noexcept {
  assert(output != nullptr);
  constexpr std::uint64_t scale = 1'000'000;
  if (milliseconds >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
          scale) {
    return false;
  }
  *output = static_cast<std::int64_t>(milliseconds * scale);
  return true;
}

[[nodiscard]] inline bool ParseArgs(simdjson::ondemand::object root,
                                    OperationResponse* response) noexcept {
  assert(response != nullptr);
  simdjson::ondemand::value value;
  if (!FindSimdjsonField(root, "args", &value)) {
    return false;
  }
  simdjson::ondemand::array args;
  if (value.get_array().get(args) != simdjson::SUCCESS) {
    return false;
  }

  for (simdjson::simdjson_result<simdjson::ondemand::value> element : args) {
    simdjson::ondemand::object arg;
    if (element.get_object().get(arg) != simdjson::SUCCESS) {
      return false;
    }

    if (!FindSimdjsonField(arg, "clientOid", &value)) {
      return false;
    }
    std::string_view client_oid{};
    if (!ReadSimdjsonString(value, &client_oid)) {
      return false;
    }
    response->client_oid = ClientOidCodec::Parse(client_oid);
    if (!response->client_oid.ok) {
      return false;
    }

    if (FindSimdjsonField(arg, "orderId", &value)) {
      bool is_null = false;
      if (value.is_null().get(is_null) != simdjson::SUCCESS) {
        return false;
      }
      if (!is_null && !ReadUint64(value, &response->exchange_order_id)) {
        return false;
      }
    }
    if (FindSimdjsonField(arg, "cTime", &value)) {
      std::uint64_t creation_time = 0;
      if (!ReadUint64(value, &creation_time) ||
          creation_time > static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max())) {
        return false;
      }
      response->creation_time_ms = static_cast<std::int64_t>(creation_time);
    }
    return true;
  }
  return false;
}

[[nodiscard]] inline OperationResponse ParseOperationResponseDocument(
    simdjson::ondemand::document document) noexcept {
  OperationResponse response{};
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    return response;
  }

  simdjson::ondemand::value value;
  std::string_view event{};
  if (!FindSimdjsonField(root, "event", &value) ||
      !ReadSimdjsonString(value, &event)) {
    return response;
  }

  std::uint64_t code = 0;
  if (!FindSimdjsonField(root, "code", &value) || !ReadUint64(value, &code) ||
      code > std::numeric_limits<std::uint32_t>::max()) {
    return response;
  }
  response.error_code = static_cast<std::uint32_t>(code);
  if (FindSimdjsonField(root, "msg", &value)) {
    std::string_view error_message{};
    if (ReadSimdjsonString(value, &error_message)) {
      response.error_message_hash = HashBitgetOperationString(error_message);
    }
  }

  if (event == "login") {
    if (code != 0) {
      return response;
    }
    response.kind = OperationResponseKind::kLoginAccepted;
    response.parse_status = OperationParseStatus::kOk;
    return response;
  }

  const bool has_id = FindSimdjsonField(root, "id", &value);
  if (!has_id) {
    if (event == "error" && code != 0) {
      response.kind = OperationResponseKind::kLoginRejected;
      response.parse_status = OperationParseStatus::kOk;
    }
    return response;
  }

  std::uint64_t encoded_request_id = 0;
  if (!ReadUint64(value, &encoded_request_id)) {
    return response;
  }
  response.request_id = RequestIdCodec::Decode(encoded_request_id);
  if (!response.request_id.ok) {
    return response;
  }

  const bool has_topic = FindSimdjsonField(root, "topic", &value);
  if (has_topic) {
    std::string_view topic{};
    if (!ReadSimdjsonString(value, &topic)) {
      return response;
    }
    response.topic = ParseTopic(topic);
    if (!TopicMatchesRequest(response.topic, response.request_id.type)) {
      return response;
    }
  } else if (event != "error" ||
             !IsDocumentedTopiclessPlaceAmbiguity(response.error_code,
                                                  response.request_id.type)) {
    return response;
  }

  if (FindSimdjsonField(root, "connId", &value)) {
    std::string_view connection_id{};
    if (ReadSimdjsonString(value, &connection_id)) {
      response.connection_id_hash = HashBitgetOperationString(connection_id);
    }
  }
  if (FindSimdjsonField(root, "ts", &value)) {
    std::uint64_t exchange_ms = 0;
    if (!ReadUint64(value, &exchange_ms) ||
        !ScaleMillisecondsToNanoseconds(exchange_ms, &response.exchange_ns)) {
      return response;
    }
  } else if (code == 0) {
    return response;
  }

  if (code == 0) {
    if (event != "trade" || !has_topic || !ParseArgs(root, &response)) {
      return response;
    }
    response.kind = OperationResponseKind::kAck;
  } else {
    if (event != "error" && event != "trade") {
      return response;
    }
    if (!IsDefiniteBusinessReject(response.error_code)) {
      response.kind = OperationResponseKind::kUnknownResult;
    } else if (response.request_id.type == OrderRequestType::kPlaceOrder) {
      response.kind = OperationResponseKind::kRejected;
    } else {
      response.kind = OperationResponseKind::kCancelRejected;
    }
  }

  response.parse_status = OperationParseStatus::kOk;
  return response;
}

[[nodiscard]] inline OperationResponse ClassifyDeferredJsonError(
    std::string_view payload, OperationResponse response) noexcept {
  if (response.parse_status != OperationParseStatus::kUnexpectedShape) {
    return response;
  }
  simdjson::padded_string padded(payload);
  simdjson::dom::parser validator;
  simdjson::dom::element element;
  if (validator.parse(padded).get(element) != simdjson::SUCCESS) {
    response.parse_status = OperationParseStatus::kInvalidJson;
  }
  return response;
}

}  // namespace detail

[[nodiscard]] inline OperationResponse ParseOperationResponse(
    std::string_view payload, std::uint32_t readable_tail_bytes,
    simdjson::ondemand::parser& parser) noexcept {
  if (payload.empty()) {
    return {.parse_status = OperationParseStatus::kInvalidJson};
  }
  simdjson::ondemand::document document;
  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    const simdjson::padded_string_view view(
        payload.data(), payload.size(), payload.size() + readable_tail_bytes);
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      return {.parse_status = OperationParseStatus::kInvalidJson};
    }
    return detail::ClassifyDeferredJsonError(
        payload, detail::ParseOperationResponseDocument(std::move(document)));
  }

  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return {.parse_status = OperationParseStatus::kInvalidJson};
  }
  return detail::ClassifyDeferredJsonError(
      payload, detail::ParseOperationResponseDocument(std::move(document)));
}

[[nodiscard]] inline OperationResponse ParseOperationResponse(
    std::string_view payload) noexcept {
  simdjson::ondemand::parser parser;
  return ParseOperationResponse(payload, 0, parser);
}

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_OPERATION_RESPONSE_PARSER_H_
