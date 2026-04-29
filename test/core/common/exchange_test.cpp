#include "core/common/constants.h"
#include "core/common/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>
#include <magic_enum/magic_enum.hpp>

namespace {

TEST(CoreExchangeTest, DefinesSupportedExchangeSet) {
  using aquila::core::Exchange;

  constexpr std::array<Exchange, 6> kExchanges = {
      Exchange::kBinance, Exchange::kOkx,    Exchange::kGate,
      Exchange::kBybit,   Exchange::kBitget, Exchange::kCoinbase,
  };
  constexpr std::array<std::string_view, 6> kNames = {
      "kBinance", "kOkx",    "kGate",
      "kBybit",   "kBitget", "kCoinbase",
  };

  static_assert(magic_enum::enum_count<Exchange>() == kExchanges.size());
  for (size_t i = 0; i < kExchanges.size(); ++i) {
    EXPECT_EQ(magic_enum::enum_name(kExchanges[i]), kNames[i]);
    EXPECT_EQ(static_cast<std::uint8_t>(kExchanges[i]), i);
  }
}

}  // namespace
