#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_TEXT_ENVELOPE_PARSER_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_TEXT_ENVELOPE_PARSER_H_

#include <cstdint>
#include <string_view>
#include <utility>

#include "exchange/gate/common/simdjson_utils.h"
#include <simdjson.h>

namespace aquila::gate::detail {

enum class TextEvent : std::uint8_t {
  kUnknown = 0,
  kSubscribe,
  kUnsubscribe,
  kUpdate,
};

struct TextEnvelope {
  TextEvent event{TextEvent::kUnknown};
  bool channel_is_book_ticker{false};
  bool result_success{false};
  bool has_error{false};
};

inline TextEvent ParseTextEvent(std::string_view event) noexcept {
  if (event == "subscribe") {
    return TextEvent::kSubscribe;
  }
  if (event == "unsubscribe") {
    return TextEvent::kUnsubscribe;
  }
  if (event == "update") {
    return TextEvent::kUpdate;
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
  if (FindSimdjsonField(root, "channel", &value)) {
    std::string_view channel{};
    envelope.channel_is_book_ticker =
        ReadSimdjsonString(value, &channel) && channel == "futures.book_ticker";
  }
  if (FindSimdjsonField(root, "event", &value)) {
    std::string_view event{};
    if (ReadSimdjsonString(value, &event)) {
      envelope.event = ParseTextEvent(event);
    }
  }
  envelope.has_error = FindSimdjsonField(root, "error", &value);

  simdjson::ondemand::object result;
  if (FindSimdjsonObject(root, "result", &result) &&
      FindSimdjsonField(result, "status", &value)) {
    std::string_view status{};
    envelope.result_success =
        ReadSimdjsonString(value, &status) && status == "success";
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

}  // namespace aquila::gate::detail

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_TEXT_ENVELOPE_PARSER_H_
