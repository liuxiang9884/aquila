#ifndef AQUILA_EXCHANGE_GATE_COMMON_SIMDJSON_UTILS_H_
#define AQUILA_EXCHANGE_GATE_COMMON_SIMDJSON_UTILS_H_

#include <string_view>
#include <utility>

#include <simdjson.h>

namespace aquila::gate::detail {

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

inline bool FindSimdjsonField(simdjson::ondemand::object object,
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

inline bool FindSimdjsonObject(simdjson::ondemand::object object,
                               std::string_view key,
                               simdjson::ondemand::object* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  simdjson::ondemand::value value;
  if (!FindSimdjsonField(object, key, &value)) {
    return false;
  }
  simdjson::ondemand::object nested;
  if (value.get_object().get(nested) != simdjson::SUCCESS) {
    return false;
  }
  *output = nested;
  return true;
}

}  // namespace aquila::gate::detail

#endif  // AQUILA_EXCHANGE_GATE_COMMON_SIMDJSON_UTILS_H_
