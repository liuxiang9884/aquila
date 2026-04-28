#ifndef AQUILA_EXCHANGE_GATE_TRADING_SUBMIT_RESPONSE_PARSER_H_
#define AQUILA_EXCHANGE_GATE_TRADING_SUBMIT_RESPONSE_PARSER_H_

#include <charconv>
#include <cstdint>
#include <utility>
#include <span>
#include <string_view>

#include <simdjson.h>
#include <yyjson.h>

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

class JsonDoc {
 public:
  explicit JsonDoc(yyjson_doc* doc) noexcept : doc_(doc) {}
  JsonDoc(const JsonDoc&) = delete;
  JsonDoc& operator=(const JsonDoc&) = delete;
  ~JsonDoc() {
    if (doc_ != nullptr) {
      yyjson_doc_free(doc_);
    }
  }

  [[nodiscard]] yyjson_doc* get() const noexcept { return doc_; }

 private:
  yyjson_doc* doc_{nullptr};
};

inline std::string_view ReadStringView(yyjson_val* value) noexcept {
  if (!yyjson_is_str(value)) {
    return {};
  }
  const char* text = yyjson_get_str(value);
  if (text == nullptr) {
    return {};
  }
  return std::string_view(text, yyjson_get_len(value));
}

inline bool ReadBool(yyjson_val* value, bool* output) noexcept {
  if (output == nullptr || !yyjson_is_bool(value)) {
    return false;
  }
  *output = yyjson_get_bool(value);
  return true;
}

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

inline bool ReadUint64(yyjson_val* value, std::uint64_t* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  if (yyjson_is_uint(value)) {
    *output = yyjson_get_uint(value);
    return true;
  }
  if (yyjson_is_int(value)) {
    const std::int64_t signed_value = yyjson_get_sint(value);
    if (signed_value < 0) {
      return false;
    }
    *output = static_cast<std::uint64_t>(signed_value);
    return true;
  }
  return ParseUintString(ReadStringView(value), output);
}

inline std::uint16_t ReadStatusCode(yyjson_val* value) noexcept {
  std::uint64_t parsed = 0;
  if (!ReadUint64(value, &parsed) || parsed > 999) {
    return 0;
  }
  return static_cast<std::uint16_t>(parsed);
}

inline std::uint64_t HashStringValue(yyjson_val* value) noexcept {
  const std::string_view text = ReadStringView(value);
  return text.empty() ? 0 : HashGateSubmitString(text);
}

inline GateSubmitResponse ParseDocument(yyjson_doc* doc) noexcept {
  GateSubmitResponse response{};
  if (doc == nullptr) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  yyjson_val* root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    response.parse_status = GateSubmitParseStatus::kUnexpectedShape;
    return response;
  }

  response.parse_status = GateSubmitParseStatus::kOk;
  response.request_id_hash = HashStringValue(yyjson_obj_get(root, "request_id"));

  bool ack = false;
  response.has_ack = ReadBool(yyjson_obj_get(root, "ack"), &ack);
  response.ack = response.has_ack && ack;

  yyjson_val* header = yyjson_obj_get(root, "header");
  if (yyjson_is_obj(header)) {
    response.http_status = ReadStatusCode(yyjson_obj_get(header, "status"));
    response.channel_is_order_place =
        ReadStringView(yyjson_obj_get(header, "channel")) ==
        std::string_view("futures.order_place");
  }

  yyjson_val* data = yyjson_obj_get(root, "data");
  if (!yyjson_is_obj(data)) {
    return response;
  }

  yyjson_val* errs = yyjson_obj_get(data, "errs");
  if (yyjson_is_obj(errs)) {
    response.kind = GateSubmitResponseKind::kError;
    response.error_label_hash = HashStringValue(yyjson_obj_get(errs, "label"));
    return response;
  }

  yyjson_val* result = yyjson_obj_get(data, "result");
  if (!yyjson_is_obj(result)) {
    return response;
  }

  if (response.ack) {
    response.kind = GateSubmitResponseKind::kAck;
    response.req_id_hash = HashStringValue(yyjson_obj_get(result, "req_id"));
    return response;
  }

  response.kind = GateSubmitResponseKind::kResult;
  (void)ReadUint64(yyjson_obj_get(result, "id"),
                   &response.exchange_order_id);
  response.text_hash = HashStringValue(yyjson_obj_get(result, "text"));
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

inline bool FindField(simdjson::ondemand::object object,
                      std::string_view key,
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

inline bool FindObject(simdjson::ondemand::object object,
                       std::string_view key,
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

}  // namespace detail

inline GateSubmitResponse ParseGateSubmitResponse(
    std::string_view payload,
    const yyjson_alc* allocator = nullptr) noexcept {
  GateSubmitResponse response{};
  if (payload.empty()) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  yyjson_read_err error{};
  detail::JsonDoc doc(yyjson_read_opts(
      const_cast<char*>(payload.data()), payload.size(), YYJSON_READ_NOFLAG,
      allocator, &error));
  if (doc.get() == nullptr) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  return detail::ParseDocument(doc.get());
}

inline GateSubmitResponse ParseGateSubmitResponseInsitu(
    std::span<char> padded_payload,
    size_t payload_size,
    const yyjson_alc* allocator = nullptr) noexcept {
  GateSubmitResponse response{};
  if (payload_size == 0 || payload_size > padded_payload.size() ||
      padded_payload.size() - payload_size < YYJSON_PADDING_SIZE) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  yyjson_read_err error{};
  detail::JsonDoc doc(yyjson_read_opts(
      padded_payload.data(), payload_size, YYJSON_READ_INSITU, allocator,
      &error));
  if (doc.get() == nullptr) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  return detail::ParseDocument(doc.get());
}

inline GateSubmitResponse ParseGateSubmitResponseSimdjson(
    std::span<char> padded_payload,
    size_t payload_size,
    simdjson::ondemand::parser& parser) noexcept {
  GateSubmitResponse response{};
  if (payload_size == 0 || payload_size > padded_payload.size() ||
      padded_payload.size() - payload_size < simdjson::SIMDJSON_PADDING) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  simdjson::padded_string_view view(padded_payload.data(), payload_size,
                                    padded_payload.size());
  simdjson::ondemand::document document;
  if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  return detail::ParseSimdjsonDocument(std::move(document));
}

}  // namespace aquila::exchange::gate::trading

#endif  // AQUILA_EXCHANGE_GATE_TRADING_SUBMIT_RESPONSE_PARSER_H_
