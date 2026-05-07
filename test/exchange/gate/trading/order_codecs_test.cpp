#include "exchange/gate/trading/order_codecs.h"

#include <array>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

namespace aquila::gate {
namespace {

TEST(RequestIdCodecTest, EncodesTypeInHighEightBits) {
  constexpr std::uint64_t sequence = 42;

  const std::uint64_t encoded =
      RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, sequence);
  const DecodedRequestId decoded = RequestIdCodec::Decode(encoded);

  EXPECT_TRUE(decoded.ok);
  EXPECT_EQ(decoded.type, OrderRequestType::kPlaceOrder);
  EXPECT_EQ(decoded.sequence, sequence);
  EXPECT_EQ(encoded >> 56, 2U);
}

TEST(RequestIdCodecTest, MasksSequenceToLowFiftySixBits) {
  constexpr std::uint64_t sequence = 0x1FFFFFFFFFFFFFFULL;

  const DecodedRequestId decoded = RequestIdCodec::Decode(
      RequestIdCodec::Encode(OrderRequestType::kCancelOrder, sequence));

  EXPECT_TRUE(decoded.ok);
  EXPECT_EQ(decoded.type, OrderRequestType::kCancelOrder);
  EXPECT_EQ(decoded.sequence, 0x00FFFFFFFFFFFFFFULL);
}

TEST(RequestIdCodecTest, RejectsUnknownRequestType) {
  const DecodedRequestId decoded =
      RequestIdCodec::Decode(0x7F00000000000001ULL);

  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.type, OrderRequestType::kUnknown);
  EXPECT_EQ(decoded.sequence, 1U);
}

TEST(OrderTextCodecTest, FormatsAndParsesStandardOrderText) {
  std::array<char, 32> buffer{};

  const std::string_view text = OrderTextCodec::Format(123456789, buffer);
  const ParsedOrderText parsed = OrderTextCodec::Parse(text);

  EXPECT_EQ(text, "t-123456789");
  EXPECT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.local_order_id, 123456789);
}

TEST(OrderTextCodecTest, RejectsUnsupportedPrefix) {
  const ParsedOrderText parsed = OrderTextCodec::Parse("ao-100");

  EXPECT_FALSE(parsed.ok);
  EXPECT_EQ(parsed.local_order_id, 0);
}

TEST(OrderTextCodecTest, ReportsTooSmallFormatBuffer) {
  std::array<char, 4> buffer{};

  const std::string_view text = OrderTextCodec::Format(123456789, buffer);

  EXPECT_TRUE(text.empty());
}

TEST(OrderTextCodecTest, DoesNotFormatNonPositiveOrderId) {
  std::array<char, 32> zero_buffer{};
  std::array<char, 32> negative_buffer{};

  const std::string_view zero_text = OrderTextCodec::Format(0, zero_buffer);
  const std::string_view negative_text =
      OrderTextCodec::Format(-1, negative_buffer);

  EXPECT_TRUE(zero_text.empty());
  EXPECT_TRUE(negative_text.empty());
}

TEST(OrderTextCodecTest, RejectsNonPositiveOrderId) {
  const ParsedOrderText parsed = OrderTextCodec::Parse("t-0");

  EXPECT_FALSE(parsed.ok);
  EXPECT_EQ(parsed.local_order_id, 0);
}

TEST(OrderTextCodecTest, RejectsNegativeOrderId) {
  const ParsedOrderText parsed = OrderTextCodec::Parse("t--1");

  EXPECT_FALSE(parsed.ok);
  EXPECT_EQ(parsed.local_order_id, 0);
}

TEST(OrderTextCodecTest, RejectsTrailingJunk) {
  const ParsedOrderText parsed = OrderTextCodec::Parse("t-12x");

  EXPECT_FALSE(parsed.ok);
  EXPECT_EQ(parsed.local_order_id, 0);
}

}  // namespace
}  // namespace aquila::gate
