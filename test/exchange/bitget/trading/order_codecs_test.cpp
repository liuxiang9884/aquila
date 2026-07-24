#include "exchange/bitget/trading/order_codecs.h"

#include <array>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace aquila::bitget {
namespace {

ClientOidRunNamespace RunNamespace(std::string_view text) {
  const std::optional<ClientOidRunNamespace> parsed =
      ClientOidRunNamespace::Parse(text);
  EXPECT_TRUE(parsed.has_value());
  return parsed.value_or(ClientOidRunNamespace{});
}

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
  const ClientOidRunNamespace run_namespace = RunNamespace("0123456789AB");

  EXPECT_EQ(ClientOidCodec::Format(run_namespace, 42, buffer),
            "a1-0123456789AB-0000000000016");
  const ParsedClientOid parsed =
      ClientOidCodec::Parse("a1-0123456789AB-0000000000016");
  ASSERT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.run_namespace, run_namespace);
  EXPECT_EQ(parsed.local_order_id, 42U);

  const std::string_view maximum = ClientOidCodec::Format(
      run_namespace, std::numeric_limits<std::uint64_t>::max(), buffer);
  EXPECT_EQ(maximum, "a1-0123456789AB-3W5E11264SGSF");
  const ParsedClientOid parsed_maximum = ClientOidCodec::Parse(maximum);
  ASSERT_TRUE(parsed_maximum.ok);
  EXPECT_EQ(parsed_maximum.run_namespace, run_namespace);
  EXPECT_EQ(parsed_maximum.local_order_id,
            std::numeric_limits<std::uint64_t>::max());
}

TEST(BitgetOrderCodecsTest, ClientOidRejectsInvalidInput) {
  const ClientOidRunNamespace run_namespace = RunNamespace("0123456789AB");
  const ClientOidRunNamespace reserved_namespace = RunNamespace("000000000000");
  std::array<char, 28> small_buffer{};

  EXPECT_TRUE(ClientOidCodec::Format(run_namespace, 0, small_buffer).empty());
  EXPECT_TRUE(ClientOidCodec::Format(run_namespace, 123, small_buffer).empty());
  std::array<char, 32> buffer{};
  EXPECT_TRUE(ClientOidCodec::Format(reserved_namespace, 123, buffer).empty());
  EXPECT_FALSE(ClientOidCodec::Parse("").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a1-0123456789AB-0000000000000").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a1-0123456789AB-000000000001").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a1-0123456789AB-00000000000001").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a2-0123456789AB-0000000000001").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a1-0123456789AI-0000000000001").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a1-0123456789ab-0000000000001").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a1-0123456789AB_0000000000001").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a1-0123456789AB-000000000000!").ok);
  EXPECT_FALSE(ClientOidCodec::Parse("a1-0123456789AB-3W5E11264SGSG").ok);
}

TEST(BitgetOrderCodecsTest, RunNamespaceValidatesFixedCrockfordBase32) {
  EXPECT_TRUE(ClientOidRunNamespace::Parse("0123456789AB").has_value());
  EXPECT_TRUE(ClientOidRunNamespace::Parse("CDEFGHJKMNPQ").has_value());
  EXPECT_TRUE(ClientOidRunNamespace::Parse("RSTVWXYZ0123").has_value());
  EXPECT_FALSE(ClientOidRunNamespace::Parse("").has_value());
  EXPECT_FALSE(ClientOidRunNamespace::Parse("0123456789A").has_value());
  EXPECT_FALSE(ClientOidRunNamespace::Parse("0123456789ABC").has_value());
  EXPECT_FALSE(ClientOidRunNamespace::Parse("0123456789AI").has_value());
  EXPECT_FALSE(ClientOidRunNamespace::Parse("0123456789AO").has_value());
  EXPECT_FALSE(ClientOidRunNamespace::Parse("0123456789AU").has_value());
  EXPECT_FALSE(ClientOidRunNamespace::Parse("0123456789ab").has_value());
}

}  // namespace
}  // namespace aquila::bitget
