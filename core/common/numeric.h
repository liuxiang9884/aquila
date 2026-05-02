#ifndef AQUILA_CORE_COMMON_NUMERIC_H_
#define AQUILA_CORE_COMMON_NUMERIC_H_

#include <cassert>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <type_traits>

#include <fast_float/fast_float.h>

namespace aquila {

template <typename T>
[[nodiscard]] inline T ToNumeric(const char* start, const char* end) noexcept {
  static_assert((std::is_integral_v<T> && !std::is_same_v<T, bool>) ||
                    std::is_same_v<T, float> || std::is_same_v<T, double>,
                "ToNumeric supports integer, float, and double types");

  T parsed;
  [[maybe_unused]] const auto result =
      fast_float::from_chars(start, end, parsed);
  assert(result.ec == std::errc{} && result.ptr == end);
  return parsed;
}

template <typename T>
[[nodiscard]] inline T ToNumeric(std::string_view text) noexcept {
  return ToNumeric<T>(text.data(), text.data() + text.size());
}

[[nodiscard]] inline std::int32_t ToInt32(const char* start,
                                          const char* end) noexcept {
  return ToNumeric<std::int32_t>(start, end);
}

[[nodiscard]] inline std::int32_t ToInt32(std::string_view text) noexcept {
  return ToNumeric<std::int32_t>(text);
}

[[nodiscard]] inline std::int64_t ToInt64(const char* start,
                                          const char* end) noexcept {
  return ToNumeric<std::int64_t>(start, end);
}

[[nodiscard]] inline std::int64_t ToInt64(std::string_view text) noexcept {
  return ToNumeric<std::int64_t>(text);
}

[[nodiscard]] inline std::uint32_t ToUint32(const char* start,
                                            const char* end) noexcept {
  return ToNumeric<std::uint32_t>(start, end);
}

[[nodiscard]] inline std::uint32_t ToUint32(std::string_view text) noexcept {
  return ToNumeric<std::uint32_t>(text);
}

[[nodiscard]] inline std::uint64_t ToUint64(const char* start,
                                            const char* end) noexcept {
  return ToNumeric<std::uint64_t>(start, end);
}

[[nodiscard]] inline std::uint64_t ToUint64(std::string_view text) noexcept {
  return ToNumeric<std::uint64_t>(text);
}

[[nodiscard]] inline float ToFloat(const char* start,
                                   const char* end) noexcept {
  return ToNumeric<float>(start, end);
}

[[nodiscard]] inline float ToFloat(std::string_view text) noexcept {
  return ToNumeric<float>(text);
}

[[nodiscard]] inline double ToDouble(const char* start,
                                     const char* end) noexcept {
  return ToNumeric<double>(start, end);
}

[[nodiscard]] inline double ToDouble(std::string_view text) noexcept {
  return ToNumeric<double>(text);
}

}  // namespace aquila

#endif  // AQUILA_CORE_COMMON_NUMERIC_H_
