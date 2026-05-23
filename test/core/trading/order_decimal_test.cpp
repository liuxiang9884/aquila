#include "core/trading/order_decimal.h"

#include <array>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

namespace aquila::core {
namespace {

std::string_view FormatForTest(std::int64_t units,
                               std::int32_t decimal_places) {
  static std::array<char, 64> buffer{};
  return FormatDecimalUnits(units, decimal_places, buffer);
}

TEST(OrderDecimalTest, FormatsDecimalUnitsWithoutFloatingPoint) {
  EXPECT_EQ(FormatForTest(1, 1), "0.1");
  EXPECT_EQ(FormatForTest(123, 2), "1.23");
  EXPECT_EQ(FormatForTest(5, 3), "0.005");
  EXPECT_EQ(FormatForTest(1, 15), "0.000000000000001");
  EXPECT_EQ(FormatForTest(100, 0), "100");
  EXPECT_EQ(FormatForTest(-123, 2), "-1.23");
}

TEST(OrderDecimalTest, WritesFixedWidthDecimalDigits) {
  std::array<char, 8> buffer{};

  char* out = WriteFixedWidthDecimalDigits(5, 3, buffer.data());
  EXPECT_EQ(std::string_view(buffer.data(), out - buffer.data()), "005");

  out = WriteFixedWidthDecimalDigits(0, 2, buffer.data());
  EXPECT_EQ(std::string_view(buffer.data(), out - buffer.data()), "00");

  out = WriteFixedWidthDecimalDigits(42, 2, buffer.data());
  EXPECT_EQ(std::string_view(buffer.data(), out - buffer.data()), "42");
}

TEST(OrderDecimalTest, WritesUnsignedDecimalDigits) {
  std::array<char, 32> buffer{};

  char* out = WriteUnsignedDecimalDigits(0, buffer.data());
  EXPECT_EQ(std::string_view(buffer.data(), out - buffer.data()), "0");

  out = WriteUnsignedDecimalDigits(102, buffer.data());
  EXPECT_EQ(std::string_view(buffer.data(), out - buffer.data()), "102");

  out = WriteUnsignedDecimalDigits(123456789, buffer.data());
  EXPECT_EQ(std::string_view(buffer.data(), out - buffer.data()), "123456789");
}

TEST(OrderDecimalTest, CalculatesQuantityUnitsFromNotionalAndPriceUnits) {
  const OpenQuantityUnitsResult result =
      CalculateOpenQuantityUnits(OpenQuantityUnitsInput{
          .notional_units = 1021,
          .notional_decimal_places = 2,
          .price_units = 1021,
          .price_decimal_places = 1,
          .multiplier_units = 1,
          .multiplier_decimal_places = 0,
          .quantity_decimal_places = 1,
          .quantity_step_units = 1,
          .min_quantity_units = 1,
      });

  ASSERT_EQ(result.status, OpenQuantityUnitsStatus::kOk);
  EXPECT_EQ(result.quantity_units, 1);
  EXPECT_EQ(FormatForTest(result.quantity_units, 1), "0.1");
}

TEST(OrderDecimalTest, FloorsQuantityUnitsToStep) {
  const OpenQuantityUnitsResult result =
      CalculateOpenQuantityUnits(OpenQuantityUnitsInput{
          .notional_units = 3700,
          .notional_decimal_places = 2,
          .price_units = 1000,
          .price_decimal_places = 1,
          .multiplier_units = 1,
          .multiplier_decimal_places = 0,
          .quantity_decimal_places = 2,
          .quantity_step_units = 5,
          .min_quantity_units = 5,
      });

  ASSERT_EQ(result.status, OpenQuantityUnitsStatus::kOk);
  EXPECT_EQ(result.quantity_units, 35);
  EXPECT_EQ(FormatForTest(result.quantity_units, 2), "0.35");
}

TEST(OrderDecimalTest, AppliesMultiplierAndMinMaxLimits) {
  const OpenQuantityUnitsResult clamped =
      CalculateOpenQuantityUnits(OpenQuantityUnitsInput{
          .notional_units = 1000,
          .notional_decimal_places = 2,
          .price_units = 1000,
          .price_decimal_places = 1,
          .multiplier_units = 1,
          .multiplier_decimal_places = 2,
          .quantity_decimal_places = 0,
          .quantity_step_units = 1,
          .min_quantity_units = 1,
          .max_quantity_units = 7,
      });

  ASSERT_EQ(clamped.status, OpenQuantityUnitsStatus::kOk);
  EXPECT_EQ(clamped.quantity_units, 7);

  const OpenQuantityUnitsResult below_min =
      CalculateOpenQuantityUnits(OpenQuantityUnitsInput{
          .notional_units = 99,
          .notional_decimal_places = 2,
          .price_units = 1000,
          .price_decimal_places = 1,
          .multiplier_units = 1,
          .multiplier_decimal_places = 0,
          .quantity_decimal_places = 1,
          .quantity_step_units = 1,
          .min_quantity_units = 1,
      });

  EXPECT_EQ(below_min.status, OpenQuantityUnitsStatus::kBelowMinimum);
}

}  // namespace
}  // namespace aquila::core
