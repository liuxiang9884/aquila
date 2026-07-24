#include "exchange/bitget/trading/operation_response_parser.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <simdjson.h>

namespace aquila::bitget {
namespace {

TEST(BitgetOperationResponseParserTest, ParsesLoginSuccessAndFailure) {
  const OperationResponse success =
      ParseOperationResponse(R"({"event":"login","code":"0","msg":""})");
  const OperationResponse failure = ParseOperationResponse(
      R"({"event":"error","code":"30005","msg":"error"})");

  ASSERT_EQ(success.parse_status, OperationParseStatus::kOk);
  EXPECT_EQ(success.kind, OperationResponseKind::kLoginAccepted);
  EXPECT_EQ(success.error_code, 0U);
  ASSERT_EQ(failure.parse_status, OperationParseStatus::kOk);
  EXPECT_EQ(failure.kind, OperationResponseKind::kLoginRejected);
  EXPECT_EQ(failure.error_code, 30005U);
}

TEST(BitgetOperationResponseParserTest, ParsesPlaceSuccessAck) {
  static constexpr std::string_view payload = R"({
    "event":"trade",
    "id":"72057594037927945",
    "category":"usdt-futures",
    "topic":"place-order",
    "args":[{"symbol":"BTCUSDT","orderId":"123456789","clientOid":"a1-0123456789AB-0000000000016","cTime":"1750034397008"}],
    "code":"0",
    "msg":"success",
    "connId":"connection-1",
    "ts":"1750034397076"
  })";

  const OperationResponse response = ParseOperationResponse(payload);

  ASSERT_EQ(response.parse_status, OperationParseStatus::kOk);
  EXPECT_EQ(response.kind, OperationResponseKind::kAck);
  EXPECT_EQ(response.topic, OperationTopic::kPlaceOrder);
  ASSERT_TRUE(response.request_id.ok);
  EXPECT_EQ(response.request_id.type, OrderRequestType::kPlaceOrder);
  EXPECT_EQ(response.request_id.sequence, 9U);
  ASSERT_TRUE(response.client_oid.ok);
  EXPECT_EQ(response.client_oid.run_namespace.View(), "0123456789AB");
  EXPECT_EQ(response.client_oid.local_order_id, 42U);
  EXPECT_EQ(response.exchange_order_id, 123456789U);
  EXPECT_EQ(response.creation_time_ms, 1750034397008LL);
  EXPECT_EQ(response.exchange_ns, 1750034397076000000LL);
  EXPECT_NE(response.connection_id_hash, 0U);
}

TEST(BitgetOperationResponseParserTest, ParsesCancelSuccessAck) {
  static constexpr std::string_view payload = R"({
    "event":"trade",
    "id":"144115188075855882",
    "topic":"cancel-order",
    "args":[{"orderId":"123456789","clientOid":"a1-0123456789AB-0000000000016"}],
    "code":"0",
    "msg":"Success",
    "connId":"connection-1",
    "ts":"1750034870597"
  })";

  const OperationResponse response = ParseOperationResponse(payload);

  ASSERT_EQ(response.parse_status, OperationParseStatus::kOk);
  EXPECT_EQ(response.kind, OperationResponseKind::kAck);
  EXPECT_EQ(response.topic, OperationTopic::kCancelOrder);
  ASSERT_TRUE(response.request_id.ok);
  EXPECT_EQ(response.request_id.type, OrderRequestType::kCancelOrder);
  EXPECT_EQ(response.exchange_order_id, 123456789U);
  ASSERT_TRUE(response.client_oid.ok);
  EXPECT_EQ(response.client_oid.local_order_id, 42U);
}

TEST(BitgetOperationResponseParserTest, MapsDefiniteBusinessErrorsByRequest) {
  const OperationResponse place = ParseOperationResponse(
      R"({"event":"error","id":"72057594037927945","topic":"place-order","code":"25202","msg":"Insufficient balance","ts":"1750034870597"})");
  const OperationResponse cancel = ParseOperationResponse(
      R"({"event":"error","id":"144115188075855882","topic":"cancel-order","code":"25204","msg":"Order does not exist","ts":"1750034870597"})");

  EXPECT_EQ(place.kind, OperationResponseKind::kRejected);
  EXPECT_EQ(place.error_code, 25202U);
  EXPECT_NE(place.error_message_hash, 0U);
  EXPECT_EQ(cancel.kind, OperationResponseKind::kCancelRejected);
  EXPECT_EQ(cancel.error_code, 25204U);
}

TEST(BitgetOperationResponseParserTest,
     MapsDocumentedAmbiguousErrorsToUnknown) {
  for (const std::string_view code : {"40010", "40725", "45001"}) {
    const std::string payload =
        std::string{
            "{\"event\":\"error\",\"id\":\"72057594037927945\",\"code\":\""} +
        std::string{code} + "\",\"msg\":\"error\"}";
    const OperationResponse response = ParseOperationResponse(payload);
    ASSERT_EQ(response.parse_status, OperationParseStatus::kOk);
    EXPECT_EQ(response.kind, OperationResponseKind::kUnknownResult);
  }
}

TEST(BitgetOperationResponseParserTest,
     RejectsMissingTopicOutsideDocumentedPlaceAmbiguity) {
  for (
      const std::string_view payload : {
          std::string_view{
              R"({"event":"error","id":"72057594037927945","code":"25202","msg":"Insufficient balance"})"},
          std::string_view{
              R"({"event":"error","id":"72057594037927945","code":"49999","msg":"unclassified"})"},
          std::string_view{
              R"({"event":"error","id":"144115188075855881","code":"40010","msg":"Request timed out"})"},
      }) {
    EXPECT_EQ(ParseOperationResponse(payload).parse_status,
              OperationParseStatus::kUnexpectedShape)
        << payload;
  }
}

TEST(BitgetOperationResponseParserTest, MapsUnclassifiedErrorToUnknown) {
  const OperationResponse response = ParseOperationResponse(
      R"({"event":"error","id":"72057594037927945","topic":"place-order","code":"49999","msg":"unclassified"})");

  ASSERT_EQ(response.parse_status, OperationParseStatus::kOk);
  EXPECT_EQ(response.kind, OperationResponseKind::kUnknownResult);
}

TEST(BitgetOperationResponseParserTest, SupportsPaddedInput) {
  static constexpr std::string_view payload =
      R"({"event":"trade","id":"72057594037927945","topic":"place-order","args":[{"orderId":null,"clientOid":"a1-0123456789AB-0000000000016"}],"code":"0","msg":"success","ts":"1750034397076"})";
  std::array<char, payload.size() + simdjson::SIMDJSON_PADDING> scratch{};
  std::copy(payload.begin(), payload.end(), scratch.begin());
  simdjson::ondemand::parser parser;

  const OperationResponse response =
      ParseOperationResponse(std::string_view(scratch.data(), payload.size()),
                             simdjson::SIMDJSON_PADDING, parser);

  ASSERT_EQ(response.parse_status, OperationParseStatus::kOk);
  EXPECT_EQ(response.kind, OperationResponseKind::kAck);
  EXPECT_EQ(response.exchange_order_id, 0U);
}

TEST(BitgetOperationResponseParserTest, RejectsMalformedOrMissingFields) {
  EXPECT_EQ(ParseOperationResponse("{").parse_status,
            OperationParseStatus::kInvalidJson);
  EXPECT_EQ(
      ParseOperationResponse(R"({"event":"trade","code":"0"})").parse_status,
      OperationParseStatus::kUnexpectedShape);
  EXPECT_EQ(
      ParseOperationResponse(
          R"({"event":"trade","id":"72057594037927945","topic":"place-order","args":[{"clientOid":"x-42"}],"code":"0","ts":"1"})")
          .parse_status,
      OperationParseStatus::kUnexpectedShape);
  EXPECT_EQ(
      ParseOperationResponse(
          R"({"event":"trade","id":"not-a-number","topic":"place-order","args":[{"clientOid":"a1-0123456789AB-0000000000016"}],"code":"0","ts":"1"})")
          .parse_status,
      OperationParseStatus::kUnexpectedShape);
  EXPECT_EQ(
      ParseOperationResponse(
          R"({"event":"trade","id":"72057594037927945","topic":"place-order","args":[{"orderId":{},"clientOid":"a1-0123456789AB-0000000000016"}],"code":"0","ts":"1"})")
          .parse_status,
      OperationParseStatus::kUnexpectedShape);
}

TEST(BitgetOperationResponseParserTest, RejectsTimestampOverflow) {
  static constexpr std::string_view payload =
      R"({"event":"trade","id":"72057594037927945","topic":"place-order","args":[{"clientOid":"a1-0123456789AB-0000000000016"}],"code":"0","ts":"18446744073709551615"})";

  EXPECT_EQ(ParseOperationResponse(payload).parse_status,
            OperationParseStatus::kUnexpectedShape);
}

}  // namespace
}  // namespace aquila::bitget
