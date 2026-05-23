#ifndef AQUILA_CORE_TRADING_ORDER_DECIMAL_H_
#define AQUILA_CORE_TRADING_ORDER_DECIMAL_H_

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace aquila::core {

inline constexpr std::array<std::int64_t, 16> kOrderDecimalScales{
    1,
    10,
    100,
    1'000,
    10'000,
    100'000,
    1'000'000,
    10'000'000,
    100'000'000,
    1'000'000'000,
    10'000'000'000,
    100'000'000'000,
    1'000'000'000'000,
    10'000'000'000'000,
    100'000'000'000'000,
    1'000'000'000'000'000,
};
inline constexpr std::int32_t kMaxOrderDecimalPlaces =
    static_cast<std::int32_t>(kOrderDecimalScales.size()) - 1;

struct DecimalFormatResult {
  bool ok{false};
  std::string_view text{};
};

enum class OpenQuantityUnitsStatus : std::uint8_t {
  kOk,
  kInvalidInput,
  kBelowMinimum,
  kOverflow,
};

struct OpenQuantityUnitsInput {
  std::int64_t notional_units{0};
  std::int32_t notional_decimal_places{0};
  std::int64_t price_units{0};
  std::int32_t price_decimal_places{0};
  std::int64_t multiplier_units{1};
  std::int32_t multiplier_decimal_places{0};
  std::int32_t quantity_decimal_places{0};
  std::int64_t quantity_step_units{1};
  std::int64_t min_quantity_units{0};
  std::int64_t max_quantity_units{0};
};

struct OpenQuantityUnitsResult {
  OpenQuantityUnitsStatus status{OpenQuantityUnitsStatus::kInvalidInput};
  std::int64_t quantity_units{0};
};

[[nodiscard]] inline bool Pow10Int64(std::int32_t decimal_places,
                                     std::int64_t* output) noexcept {
  if (output == nullptr || decimal_places < 0 ||
      decimal_places > kMaxOrderDecimalPlaces) {
    return false;
  }
  *output = kOrderDecimalScales[static_cast<std::size_t>(decimal_places)];
  return true;
}

[[nodiscard]] inline std::uint64_t AbsMagnitude(std::int64_t value) noexcept {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

template <std::size_t N>
[[nodiscard]] inline DecimalFormatResult FormatDecimalUnits(
    std::int64_t units, std::int32_t decimal_places,
    std::array<char, N>& buffer) noexcept {
  std::int64_t scale = 1;
  if (!Pow10Int64(decimal_places, &scale) || buffer.empty()) {
    return {};
  }

  char* out = buffer.data();
  char* const end = buffer.data() + buffer.size();
  if (units < 0) {
    if (out == end) {
      return {};
    }
    *out++ = '-';
  }

  const std::uint64_t magnitude = AbsMagnitude(units);
  const std::uint64_t integer_units =
      magnitude / static_cast<std::uint64_t>(scale);
  const auto integer_result = std::to_chars(out, end, integer_units);
  if (integer_result.ec != std::errc{}) {
    return {};
  }
  out = integer_result.ptr;

  if (decimal_places == 0) {
    return {.ok = true,
            .text = std::string_view(
                buffer.data(), static_cast<std::size_t>(out - buffer.data()))};
  }

  if (out == end) {
    return {};
  }
  *out++ = '.';

  const std::uint64_t fractional_units =
      magnitude % static_cast<std::uint64_t>(scale);
  std::array<char, kMaxOrderDecimalPlaces + 1> fractional_buffer{};
  const auto fractional_result = std::to_chars(
      fractional_buffer.data(),
      fractional_buffer.data() + fractional_buffer.size(), fractional_units);
  if (fractional_result.ec != std::errc{}) {
    return {};
  }

  const std::size_t fractional_digits = static_cast<std::size_t>(
      fractional_result.ptr - fractional_buffer.data());
  if (fractional_digits > static_cast<std::size_t>(decimal_places)) {
    return {};
  }
  const std::size_t leading_zeros =
      static_cast<std::size_t>(decimal_places) - fractional_digits;
  if (static_cast<std::size_t>(end - out) < leading_zeros + fractional_digits) {
    return {};
  }
  std::memset(out, '0', leading_zeros);
  out += leading_zeros;
  std::memcpy(out, fractional_buffer.data(), fractional_digits);
  out += fractional_digits;

  return {.ok = true,
          .text = std::string_view(
              buffer.data(), static_cast<std::size_t>(out - buffer.data()))};
}

[[nodiscard]] inline bool CheckedMulPositiveInt64(
    std::int64_t lhs, std::int64_t rhs, std::int64_t* output) noexcept {
  if (output == nullptr || lhs < 0 || rhs < 0) {
    return false;
  }
  if (lhs != 0 && rhs > std::numeric_limits<std::int64_t>::max() / lhs) {
    return false;
  }
  *output = lhs * rhs;
  return true;
}

[[nodiscard]] inline OpenQuantityUnitsResult CalculateOpenQuantityUnits(
    const OpenQuantityUnitsInput& input) noexcept {
  std::int64_t notional_scale = 1;
  std::int64_t price_scale = 1;
  std::int64_t multiplier_scale = 1;
  std::int64_t quantity_scale = 1;
  if (!Pow10Int64(input.notional_decimal_places, &notional_scale) ||
      !Pow10Int64(input.price_decimal_places, &price_scale) ||
      !Pow10Int64(input.multiplier_decimal_places, &multiplier_scale) ||
      !Pow10Int64(input.quantity_decimal_places, &quantity_scale) ||
      input.notional_units <= 0 || input.price_units <= 0 ||
      input.multiplier_units <= 0 || input.quantity_step_units <= 0 ||
      input.min_quantity_units < 0 || input.max_quantity_units < 0) {
    return {.status = OpenQuantityUnitsStatus::kInvalidInput};
  }

  std::int64_t numerator = input.notional_units;
  if (!CheckedMulPositiveInt64(numerator, price_scale, &numerator) ||
      !CheckedMulPositiveInt64(numerator, multiplier_scale, &numerator) ||
      !CheckedMulPositiveInt64(numerator, quantity_scale, &numerator)) {
    return {.status = OpenQuantityUnitsStatus::kOverflow};
  }

  std::int64_t denominator = input.price_units;
  if (!CheckedMulPositiveInt64(denominator, input.multiplier_units,
                               &denominator) ||
      !CheckedMulPositiveInt64(denominator, notional_scale, &denominator)) {
    return {.status = OpenQuantityUnitsStatus::kOverflow};
  }
  if (denominator <= 0) {
    return {.status = OpenQuantityUnitsStatus::kInvalidInput};
  }

  const std::int64_t raw_units = numerator / denominator;
  if (raw_units <= 0) {
    return {.status = OpenQuantityUnitsStatus::kBelowMinimum};
  }

  std::int64_t quantity_units = raw_units;
  quantity_units =
      (quantity_units / input.quantity_step_units) * input.quantity_step_units;
  if (input.max_quantity_units > 0 &&
      quantity_units > input.max_quantity_units) {
    quantity_units = (input.max_quantity_units / input.quantity_step_units) *
                     input.quantity_step_units;
  }
  if (quantity_units <= 0 || (input.min_quantity_units > 0 &&
                              quantity_units < input.min_quantity_units)) {
    return {.status = OpenQuantityUnitsStatus::kBelowMinimum};
  }
  return {.status = OpenQuantityUnitsStatus::kOk,
          .quantity_units = quantity_units};
}

}  // namespace aquila::core

#endif  // AQUILA_CORE_TRADING_ORDER_DECIMAL_H_
