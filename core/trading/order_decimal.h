#ifndef AQUILA_CORE_TRADING_ORDER_DECIMAL_H_
#define AQUILA_CORE_TRADING_ORDER_DECIMAL_H_

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

enum class OpenQuantityUnitsStatus : std::uint8_t {
  kOk,
  kBelowMinimum,
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
  OpenQuantityUnitsStatus status{OpenQuantityUnitsStatus::kBelowMinimum};
  std::int64_t quantity_units{0};
};

[[nodiscard]] inline std::int64_t Pow10Int64(
    std::int32_t decimal_places) noexcept {
  return kOrderDecimalScales[static_cast<std::size_t>(decimal_places)];
}

[[nodiscard]] inline std::uint64_t AbsMagnitude(std::int64_t value) noexcept {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

template <std::size_t N>
[[nodiscard]] inline std::string_view FormatDecimalUnits(
    std::int64_t units, std::int32_t decimal_places,
    std::array<char, N>& buffer) noexcept {
  const std::int64_t scale = Pow10Int64(decimal_places);

  char* out = buffer.data();
  char* const end = buffer.data() + buffer.size();
  if (units < 0) {
    *out++ = '-';
  }

  const std::uint64_t magnitude = AbsMagnitude(units);
  const std::uint64_t integer_units =
      magnitude / static_cast<std::uint64_t>(scale);
  const auto integer_result = std::to_chars(out, end, integer_units);
  out = integer_result.ptr;

  if (decimal_places == 0) {
    return std::string_view(buffer.data(),
                            static_cast<std::size_t>(out - buffer.data()));
  }
  *out++ = '.';

  const std::uint64_t fractional_units =
      magnitude % static_cast<std::uint64_t>(scale);
  std::array<char, kMaxOrderDecimalPlaces + 1> fractional_buffer{};
  const auto fractional_result = std::to_chars(
      fractional_buffer.data(),
      fractional_buffer.data() + fractional_buffer.size(), fractional_units);

  const std::size_t fractional_digits = static_cast<std::size_t>(
      fractional_result.ptr - fractional_buffer.data());
  const std::size_t leading_zeros =
      static_cast<std::size_t>(decimal_places) - fractional_digits;
  std::memset(out, '0', leading_zeros);
  out += leading_zeros;
  std::memcpy(out, fractional_buffer.data(), fractional_digits);
  out += fractional_digits;

  return std::string_view(buffer.data(),
                          static_cast<std::size_t>(out - buffer.data()));
}

[[nodiscard]] inline OpenQuantityUnitsResult CalculateOpenQuantityUnits(
    const OpenQuantityUnitsInput& input) noexcept {
  const std::int64_t notional_scale = Pow10Int64(input.notional_decimal_places);
  const std::int64_t price_scale = Pow10Int64(input.price_decimal_places);
  const std::int64_t multiplier_scale =
      Pow10Int64(input.multiplier_decimal_places);
  const std::int64_t quantity_scale = Pow10Int64(input.quantity_decimal_places);

  std::int64_t numerator = input.notional_units;
  numerator *= price_scale;
  numerator *= multiplier_scale;
  numerator *= quantity_scale;

  std::int64_t denominator = input.price_units;
  denominator *= input.multiplier_units;
  denominator *= notional_scale;

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
