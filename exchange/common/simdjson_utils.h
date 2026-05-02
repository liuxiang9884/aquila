#ifndef AQUILA_EXCHANGE_COMMON_SIMDJSON_UTILS_H_
#define AQUILA_EXCHANGE_COMMON_SIMDJSON_UTILS_H_

#include <cassert>
#include <cstdint>
#include <string_view>
#include <utility>

#include <simdjson.h>

namespace aquila::exchange::detail {

inline bool ReadSimdjsonString(simdjson::ondemand::value value,
                               std::string_view* output) noexcept {
  assert(output != nullptr);
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
  assert(output != nullptr);
  bool parsed;
  if (value.get_bool().get(parsed) != simdjson::SUCCESS) {
    return false;
  }
  *output = parsed;
  return true;
}

inline bool ReadSimdjsonInt64(simdjson::ondemand::value value,
                              std::int64_t* output) noexcept {
  assert(output != nullptr);
  std::int64_t parsed;
  if (value.get_int64().get(parsed) != simdjson::SUCCESS) {
    return false;
  }
  *output = parsed;
  return true;
}

inline bool ReadSimdjsonUint64(simdjson::ondemand::value value,
                               std::uint64_t* output) noexcept {
  assert(output != nullptr);
  std::uint64_t parsed;
  if (value.get_uint64().get(parsed) != simdjson::SUCCESS) {
    return false;
  }
  *output = parsed;
  return true;
}

inline bool FindSimdjsonField(simdjson::ondemand::object object,
                              std::string_view key,
                              simdjson::ondemand::value* output) noexcept {
  assert(output != nullptr);
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
  assert(output != nullptr);
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

}  // namespace aquila::exchange::detail

#endif  // AQUILA_EXCHANGE_COMMON_SIMDJSON_UTILS_H_
