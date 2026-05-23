#include <array>
#include <charconv>
#include <cstdint>
#include <system_error>

#include <benchmark/benchmark.h>

#include "core/trading/order_decimal.h"

namespace aquila::core {
namespace {

void BM_CalculateOpenQuantityUnits(benchmark::State& state) {
  const auto notional_units = static_cast<std::int64_t>(state.range(0));
  const auto price_units = static_cast<std::int64_t>(state.range(1));
  const OpenQuantityUnitsInput input{
      .notional_units = notional_units,
      .notional_decimal_places = 2,
      .price_units = price_units,
      .price_decimal_places = 1,
      .multiplier_units = 1,
      .multiplier_decimal_places = 0,
      .quantity_decimal_places = 1,
      .quantity_step_units = 1,
      .min_quantity_units = 1,
  };

  for (auto _ : state) {
    const OpenQuantityUnitsResult result = CalculateOpenQuantityUnits(input);
    if (result.status != OpenQuantityUnitsStatus::kOk ||
        result.quantity_units != 1) {
      state.SkipWithError("quantity calculation failed");
      return;
    }
    std::int64_t quantity_units = result.quantity_units;
    benchmark::DoNotOptimize(quantity_units);
  }
}

void BM_FormatDecimalUnitsQuantity(benchmark::State& state) {
  const auto units = static_cast<std::int64_t>(state.range(0));
  const auto decimal_places = static_cast<std::int32_t>(state.range(1));
  std::array<char, 64> buffer{};

  for (auto _ : state) {
    const DecimalFormatResult result =
        FormatDecimalUnits(units, decimal_places, buffer);
    if (!result.ok) {
      state.SkipWithError("quantity formatting failed");
      return;
    }
    benchmark::DoNotOptimize(result.text.data());
    benchmark::DoNotOptimize(result.text.size());
    benchmark::ClobberMemory();
  }
}

void BM_FormatDecimalUnitsPrice(benchmark::State& state) {
  const auto units = static_cast<std::int64_t>(state.range(0));
  const auto decimal_places = static_cast<std::int32_t>(state.range(1));
  std::array<char, 64> buffer{};

  for (auto _ : state) {
    const DecimalFormatResult result =
        FormatDecimalUnits(units, decimal_places, buffer);
    if (!result.ok) {
      state.SkipWithError("price formatting failed");
      return;
    }
    benchmark::DoNotOptimize(result.text.data());
    benchmark::DoNotOptimize(result.text.size());
    benchmark::ClobberMemory();
  }
}

void BM_FormatDoubleFixedQuantity(benchmark::State& state) {
  const double value = static_cast<double>(state.range(0)) / 10.0;
  const auto precision = static_cast<int>(state.range(1));
  std::array<char, 64> buffer{};

  for (auto _ : state) {
    auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                value, std::chars_format::fixed, precision);
    if (result.ec != std::errc{}) {
      state.SkipWithError("double quantity formatting failed");
      return;
    }
    benchmark::DoNotOptimize(buffer.data());
    char* ptr = result.ptr;
    benchmark::DoNotOptimize(ptr);
    benchmark::ClobberMemory();
  }
}

void BM_FormatDoubleFixedPrice(benchmark::State& state) {
  const double value = static_cast<double>(state.range(0)) / 10.0;
  const auto precision = static_cast<int>(state.range(1));
  std::array<char, 64> buffer{};

  for (auto _ : state) {
    auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                value, std::chars_format::fixed, precision);
    if (result.ec != std::errc{}) {
      state.SkipWithError("double price formatting failed");
      return;
    }
    benchmark::DoNotOptimize(buffer.data());
    char* ptr = result.ptr;
    benchmark::DoNotOptimize(ptr);
    benchmark::ClobberMemory();
  }
}

BENCHMARK(BM_CalculateOpenQuantityUnits)
    ->Args({1021, 1021})
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_FormatDecimalUnitsQuantity)
    ->Args({1, 1})
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_FormatDecimalUnitsPrice)
    ->Args({1021, 1})
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_FormatDoubleFixedQuantity)
    ->Args({1, 1})
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_FormatDoubleFixedPrice)
    ->Args({1021, 1})
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila::core
