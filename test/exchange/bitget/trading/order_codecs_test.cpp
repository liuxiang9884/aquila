#include "exchange/bitget/trading/order_codecs.h"

#include <array>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace aquila::bitget {
namespace {

TEST(BitgetOrderCodecsTest, RequestIdRoundTripsPlaceAndCancel) {
  const std::uint64_t place =
      RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, 9);
  const std::uint64_t cancel =
      RequestIdCodec::Encode(OrderRequestType::kCancelOrder, 10);

  const DecodedRequestId decoded_place = RequestIdCodec::Decode(place);
  const DecodedRequestId decoded_cancel = RequestIdCodec::Decode(cancel);

  ASSERT_TRUE(decoded_place.ok);
  EXPECT_EQ(decoded_place.type, OrderRequestType::kPlaceOrder);
  EXPECT_EQ(decoded_place.sequence, 9U);
  ASSERT_TRUE(decoded_cancel.ok);
  EXPECT_EQ(decoded_cancel.type, OrderRequestType::kCancelOrder);
  EXPECT_EQ(decoded_cancel.sequence, 10U);
}

TEST(BitgetOrderCodecsTest, RequestIdMasksSequenceAndRejectsUnknownType) {
  const std::uint64_t encoded = RequestIdCodec::Encode(
      OrderRequestType::kPlaceOrder, std::numeric_limits<std::uint64_t>::max());
  const DecodedRequestId decoded = RequestIdCodec::Decode(encoded);

  ASSERT_TRUE(decoded.ok);
  EXPECT_EQ(decoded.sequence, RequestIdCodec::kSequenceMask);
  EXPECT_FALSE(RequestIdCodec::Decode(0).ok);
  EXPECT_FALSE(RequestIdCodec::Decode(0x7F00000000000001ULL).ok);
}

TEST(BitgetOrderCodecsTest, ClientOidRoundTripsValidIds) {
  std::array<char, 32> buffer{};

  EXPECT_EQ(ClientOidCodec::Format(42, buffer), "a-42");
  const ParsedClientOid parsed = ClientOidCodec::Parse("a-42");
  ASSERT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.local_order_id, 42U);

  const std::string_view maximum =
      ClientOidCodec::Format(std::numeric_limits<std::uint64_t>::max(), buffer);
  EXPECT_EQ(maximum, "a-18446744073709551615");
  const ParsedClientOid parsed_maximum = ClientOidCodec::Parse(maximum);
  ASSERT_TRUE(parsed_maximum.ok);
  EXPECT_EQ(parsed_maximum.local_order_id,
            std::numeric_limits<std::uint64_t>::max());
}

TEST(BitgetOrderCodecsTest, ClientOidRejectsInvalidInput) {
  std::array<char, 4> small_buffer{};

  EXPECT_TRUE(ClientOidCodec::Format(0, small_buffer).empty());
  EXPECT_TRUE(ClientOidCodec::Format(123, small_buffer).empty());
  EXPECT_FALSE(ClientOidCodec::Parse("").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a-").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("x-42").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a--1").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a-0").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a-42x").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a-18446744073709551616").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a-1234567890123456789012345678901").ok);
}

}  // namespace
}  // namespace aquila::bitget
