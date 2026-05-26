#ifndef AQUILA_EXCHANGE_GATE_TRADING_SUBMIT_RESPONSE_PARSER_H_
#define AQUILA_EXCHANGE_GATE_TRADING_SUBMIT_RESPONSE_PARSER_H_

#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "exchange/gate/common/simdjson_utils.h"
#include "exchange/gate/trading/order_codecs.h"
#include <simdjson.h>

namespace aquila::gate {

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

enum GateSubmitChannel : std::uint8_t {
  kUnknown,
  kFuturesLogin,
  kFuturesOrderPlace,
  kFuturesOrderCancel,
};

enum class GateSubmitParseProfile : std::uint8_t {
  kFull,
  kOrderSession,
};

struct GateSubmitResponse {
  GateSubmitParseStatus parse_status{GateSubmitParseStatus::kUnexpectedShape};
  GateSubmitResponseKind kind{GateSubmitResponseKind::kUnknown};
  bool ack_field_present{false};
  bool has_ack{false};
  bool ack{false};
  GateSubmitChannel channel{kUnknown};
  bool channel_is_order_place{false};
  std::uint16_t http_status{0};
  std::uint64_t request_id_hash{0};
  DecodedRequestId request_id{};
  std::uint64_t req_id_hash{0};
  bool has_req_id{false};
  DecodedRequestId req_id{};
  std::uint64_t exchange_order_id{0};
  bool has_login_uid{false};
  std::uint64_t login_uid{0};
  std::uint64_t text_hash{0};
  bool has_local_order_id{false};
  std::uint64_t local_order_id{0};
  std::uint64_t error_label_hash{0};
  std::string_view error_label{};
  std::string_view error_message{};
  std::int64_t exchange_ns{0};
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

inline GateSubmitResponse InvalidJsonSubmitResponse() noexcept {
  GateSubmitResponse response{};
  response.parse_status = GateSubmitParseStatus::kInvalidJson;
  return response;
}

inline bool ParseUint64Text(std::string_view text,
                            std::uint64_t* output) noexcept {
  assert(output != nullptr);
  if (text.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  const char* const first = text.data();
  const char* const last = text.data() + text.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last) {
    return false;
  }
  *output = parsed;
  return true;
}

inline bool ReadSimdjsonUint64(simdjson::ondemand::value value,
                               std::uint64_t* output) noexcept {
  assert(output != nullptr);
  std::uint64_t unsigned_value;
  if (value.get_uint64().get(unsigned_value) == simdjson::SUCCESS) {
    *output = unsigned_value;
    return true;
  }

  std::int64_t signed_value;
  if (value.get_int64().get(signed_value) == simdjson::SUCCESS &&
      signed_value >= 0) {
    *output = static_cast<std::uint64_t>(signed_value);
    return true;
  }

  std::string_view text{};
  if (ReadSimdjsonString(value, &text) && !text.empty()) {
    return ParseUint64Text(text, output);
  }
  return false;
}

inline GateSubmitChannel ParseGateSubmitChannel(
    std::string_view channel) noexcept {
  if (channel == std::string_view("futures.login")) {
    return kFuturesLogin;
  }
  if (channel == std::string_view("futures.order_place")) {
    return kFuturesOrderPlace;
  }
  if (channel == std::string_view("futures.order_cancel")) {
    return kFuturesOrderCancel;
  }
  return kUnknown;
}

inline DecodedRequestId DecodeSimdjsonRequestId(
    simdjson::ondemand::value value) noexcept {
  std::uint64_t encoded = 0;
  if (!ReadSimdjsonUint64(value, &encoded)) {
    return {};
  }
  return RequestIdCodec::Decode(encoded);
}

template <GateSubmitParseProfile Profile>
inline void ReadSimdjsonRequestIdCorrelation(
    simdjson::ondemand::value value, std::uint64_t* hash,
    DecodedRequestId* decoded) noexcept {
  assert(hash != nullptr);
  assert(decoded != nullptr);

  std::string_view text{};
  if (ReadSimdjsonString(value, &text)) {
    if (!text.empty()) {
      if constexpr (Profile == GateSubmitParseProfile::kFull) {
        *hash = HashGateSubmitString(text);
      }
      std::uint64_t encoded = 0;
      if (ParseUint64Text(text, &encoded)) {
        *decoded = RequestIdCodec::Decode(encoded);
      }
    }
    return;
  }

  *decoded = DecodeSimdjsonRequestId(value);
}

inline std::uint16_t ReadSimdjsonStatusCode(
    simdjson::ondemand::value value) noexcept {
  std::uint64_t parsed = 0;
  if (!ReadSimdjsonUint64(value, &parsed) || parsed > 999) {
    return 0;
  }
  return static_cast<std::uint16_t>(parsed);
}

inline bool TryScaleUint64ToInt64(std::uint64_t value, std::uint64_t scale,
                                  std::int64_t* output) noexcept {
  assert(output != nullptr);
  if (scale == 0 || value > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()) /
                                scale) {
    return false;
  }
  *output = static_cast<std::int64_t>(value * scale);
  return true;
}

template <GateSubmitParseProfile Profile>
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
  if (FindSimdjsonField(root, "request_id", &value)) {
    ReadSimdjsonRequestIdCorrelation<Profile>(value, &response.request_id_hash,
                                              &response.request_id);
  }
  if (FindSimdjsonField(root, "ack", &value)) {
    response.ack_field_present = true;
    bool ack = false;
    response.has_ack = ReadSimdjsonBool(value, &ack);
    response.ack = response.has_ack && ack;
  }

  simdjson::ondemand::object header;
  if (FindSimdjsonObject(root, "header", &header)) {
    if (FindSimdjsonField(header, "response_time", &value)) {
      std::uint64_t response_time_ms = 0;
      std::int64_t response_time_ns = 0;
      if (ReadSimdjsonUint64(value, &response_time_ms) &&
          TryScaleUint64ToInt64(response_time_ms, 1'000'000,
                                &response_time_ns)) {
        response.exchange_ns = response_time_ns;
      }
    }
    if (FindSimdjsonField(header, "status", &value)) {
      response.http_status = ReadSimdjsonStatusCode(value);
    }
    if (FindSimdjsonField(header, "channel", &value)) {
      std::string_view channel{};
      if (ReadSimdjsonString(value, &channel)) {
        response.channel = ParseGateSubmitChannel(channel);
        response.channel_is_order_place =
            response.channel == kFuturesOrderPlace;
      }
    }
    if (FindSimdjsonField(header, "x_out_time", &value)) {
      std::uint64_t out_time_us = 0;
      std::int64_t out_time_ns = 0;
      if (ReadSimdjsonUint64(value, &out_time_us) &&
          TryScaleUint64ToInt64(out_time_us, 1000, &out_time_ns)) {
        response.exchange_ns = out_time_ns;
      }
    }
  }

  simdjson::ondemand::object data;
  if (!FindSimdjsonObject(root, "data", &data)) {
    return response;
  }
  const bool can_parse_error =
      response.has_ack ? !response.ack : response.http_status >= 400;
  const bool can_parse_missing_ack_result =
      !response.ack_field_present && response.http_status == 200 &&
      (response.channel == kFuturesOrderPlace ||
       response.channel == kFuturesOrderCancel);
  if (response.ack_field_present && !response.has_ack) {
    return response;
  }
  if (!response.has_ack && response.channel != kFuturesLogin &&
      !can_parse_error && !can_parse_missing_ack_result) {
    return response;
  }

  simdjson::ondemand::object errs;
  if (can_parse_error && FindSimdjsonObject(data, "errs", &errs)) {
    response.kind = GateSubmitResponseKind::kError;
    if (FindSimdjsonField(errs, "label", &value)) {
      if (ReadSimdjsonString(value, &response.error_label) &&
          !response.error_label.empty()) {
        response.error_label_hash = HashGateSubmitString(response.error_label);
      }
    }
    if (FindSimdjsonField(errs, "message", &value)) {
      (void)ReadSimdjsonString(value, &response.error_message);
    }
    return response;
  }

  simdjson::ondemand::object result;
  if (!FindSimdjsonObject(data, "result", &result)) {
    return response;
  }

  if (response.ack) {
    response.kind = GateSubmitResponseKind::kAck;
    if (FindSimdjsonField(result, "req_id", &value)) {
      ReadSimdjsonRequestIdCorrelation<Profile>(value, &response.req_id_hash,
                                                &response.req_id);
      response.has_req_id = response.req_id.ok;
    }
    return response;
  }

  response.kind = GateSubmitResponseKind::kResult;
  if (FindSimdjsonField(result, "uid", &value)) {
    response.has_login_uid = ReadSimdjsonUint64(value, &response.login_uid) &&
                             response.login_uid > 0;
  }
  if (FindSimdjsonField(result, "id", &value)) {
    (void)ReadSimdjsonUint64(value, &response.exchange_order_id);
  }
  if (FindSimdjsonField(result, "text", &value)) {
    std::string_view text{};
    if (ReadSimdjsonString(value, &text) && !text.empty()) {
      if constexpr (Profile == GateSubmitParseProfile::kFull) {
        response.text_hash = HashGateSubmitString(text);
      }
      const ParsedOrderText parsed_text = OrderTextCodec::Parse(text);
      response.has_local_order_id = parsed_text.ok;
      response.local_order_id = parsed_text.local_order_id;
    }
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
  if (FindSimdjsonField(root, "request_id", &value)) {
    ReadSimdjsonRequestIdCorrelation<GateSubmitParseProfile::kFull>(
        value, &response.request_id_hash, &response.request_id);
  }
  if (FindSimdjsonField(root, "ack", &value)) {
    response.ack_field_present = true;
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

template <GateSubmitParseProfile Profile>
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

  return detail::ParseSimdjsonDocument<Profile>(std::move(document));
}

inline GateSubmitResponse ParseGateSubmitResponse(
    std::span<char> padded_payload, size_t payload_size,
    simdjson::ondemand::parser& parser) noexcept {
  return ParseGateSubmitResponse<GateSubmitParseProfile::kFull>(
      padded_payload, payload_size, parser);
}

inline GateSubmitResponse ParseGateSubmitResponseForOrderSession(
    std::span<char> padded_payload, size_t payload_size,
    simdjson::ondemand::parser& parser) noexcept {
  return ParseGateSubmitResponse<GateSubmitParseProfile::kOrderSession>(
      padded_payload, payload_size, parser);
}

template <GateSubmitParseProfile Profile>
inline GateSubmitResponse ParseGateSubmitResponse(
    std::string_view payload, size_t readable_tail_bytes,
    simdjson::ondemand::parser& parser) noexcept {
  if (payload.empty()) {
    return detail::InvalidJsonSubmitResponse();
  }

  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    simdjson::padded_string_view view(payload.data(), payload.size(),
                                      payload.size() + readable_tail_bytes);
    simdjson::ondemand::document document;
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      return detail::InvalidJsonSubmitResponse();
    }
    return detail::ParseSimdjsonDocument<Profile>(std::move(document));
  }

  simdjson::padded_string padded(payload);
  simdjson::ondemand::document document;
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return detail::InvalidJsonSubmitResponse();
  }

  return detail::ParseSimdjsonDocument<Profile>(std::move(document));
}

inline GateSubmitResponse ParseGateSubmitResponse(
    std::string_view payload, size_t readable_tail_bytes,
    simdjson::ondemand::parser& parser) noexcept {
  return ParseGateSubmitResponse<GateSubmitParseProfile::kFull>(
      payload, readable_tail_bytes, parser);
}

inline GateSubmitResponse ParseGateSubmitResponseForOrderSession(
    std::string_view payload, size_t readable_tail_bytes,
    simdjson::ondemand::parser& parser) noexcept {
  return ParseGateSubmitResponse<GateSubmitParseProfile::kOrderSession>(
      payload, readable_tail_bytes, parser);
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

template <GateSubmitParseProfile Profile>
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

  return detail::ParseSimdjsonDocument<Profile>(std::move(document));
}

inline GateSubmitResponse ParseGateSubmitResponse(
    std::string_view payload) noexcept {
  return ParseGateSubmitResponse<GateSubmitParseProfile::kFull>(payload);
}

inline GateSubmitResponse ParseGateSubmitResponseForOrderSession(
    std::string_view payload) noexcept {
  return ParseGateSubmitResponse<GateSubmitParseProfile::kOrderSession>(
      payload);
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

// Benchmark-only simdjson wrapper names live in
// benchmark/exchange/gate/trading/submit_response_parse_benchmark.cpp.

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_SUBMIT_RESPONSE_PARSER_H_
