#include "core/utils/numeric.h"

#include <cstdint>
#include <limits>
#include <string_view>

#include <gtest/gtest.h>

namespace {

TEST(CoreNumericTest, ParsesIntegerStringViews) {
  EXPECT_EQ(aquila::ToNumeric<std::uint64_t>("400900217"), 400900217ULL);
  EXPECT_EQ(aquila::ToInt32("-42"), -42);
  EXPECT_EQ(aquila::ToUint64("18446744073709551615"),
            std::numeric_limits<std::uint64_t>::max());
}

TEST(CoreNumericTest, ParsesPointerRanges) {
  constexpr std::string_view value = "123456789";
  EXPECT_EQ(aquila::ToNumeric<std::uint64_t>(value.data(),
                                             value.data() + value.size()),
            123456789ULL);
  EXPECT_EQ(aquila::ToInt64(value.data(), value.data() + value.size()),
            123456789LL);
}

TEST(CoreNumericTest, ParsesFloatingPointAliases) {
  EXPECT_FLOAT_EQ(aquila::ToFloat("25.5"), 25.5F);
  EXPECT_DOUBLE_EQ(aquila::ToDouble("25.35190000"), 25.3519);
}

}  // namespace
