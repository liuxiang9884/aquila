#ifndef AQUILA_EXCHANGE_BITGET_MARKET_DATA_TEXT_ENVELOPE_PARSER_H_
#define AQUILA_EXCHANGE_BITGET_MARKET_DATA_TEXT_ENVELOPE_PARSER_H_

#include <cstdint>
#include <string_view>
#include <utility>

#include "exchange/common/simdjson_utils.h"
#include <simdjson.h>

namespace aquila::bitget::detail {

using ::aquila::exchange::detail::FindSimdjsonField;
using ::aquila::exchange::detail::FindSimdjsonObject;
using ::aquila::exchange::detail::ReadSimdjsonString;

enum class TextEvent : std::uint8_t {
  kSubscribe = 0,
  kUnsubscribe,
  kUnknown,
};

struct TextEnvelope {
  TextEvent event{TextEvent::kUnknown};
  bool has_error{false};
  bool result_success{false};
};

inline TextEvent ParseTextEvent(std::string_view event) noexcept {
  if (event == "subscribe") {
    return TextEvent::kSubscribe;
  }
  if (event == "unsubscribe") {
    return TextEvent::kUnsubscribe;
  }
  return TextEvent::kUnknown;
}

inline bool ParseTextEnvelopeDocument(simdjson::ondemand::document document,
                                      TextEnvelope& output) noexcept {
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    return false;
  }

  TextEnvelope envelope{};
  simdjson::ondemand::value value;
  if (FindSimdjsonField(root, "event", &value) ||
      FindSimdjsonField(root, "op", &value)) {
    std::string_view event{};
    if (ReadSimdjsonString(value, &event)) {
      envelope.event = ParseTextEvent(event);
    }
  }

  envelope.has_error = FindSimdjsonField(root, "error", &value);
  if (FindSimdjsonField(root, "code", &value)) {
    std::string_view code{};
    if (ReadSimdjsonString(value, &code)) {
      envelope.has_error = envelope.has_error || code != "0";
      envelope.result_success = code == "0";
    }
  } else if (envelope.event == TextEvent::kSubscribe ||
             envelope.event == TextEvent::kUnsubscribe) {
    envelope.result_success = !envelope.has_error;
  }

  output = envelope;
  return true;
}

inline bool ParseTextEnvelope(std::string_view payload,
                              std::uint32_t readable_tail_bytes,
                              simdjson::ondemand::parser& parser,
                              TextEnvelope& output) noexcept {
  if (payload.empty()) {
    return false;
  }

  simdjson::ondemand::document document;
  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    simdjson::padded_string_view view(payload.data(), payload.size(),
                                      payload.size() + readable_tail_bytes);
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      return false;
    }
    return ParseTextEnvelopeDocument(std::move(document), output);
  }

  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return false;
  }
  return ParseTextEnvelopeDocument(std::move(document), output);
}

}  // namespace aquila::bitget::detail

#endif  // AQUILA_EXCHANGE_BITGET_MARKET_DATA_TEXT_ENVELOPE_PARSER_H_
