#include "exchange/gate/trading/order_request_encoder.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

#include <gtest/gtest.h>

#include "exchange/gate/trading/order_signature.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::gate {
namespace {

core::OrderPlaceRequest MakePlaceRequest(
    std::uint64_t local_order_id, OrderType order_type,
    std::string_view contract, double quantity,
    std::uint8_t quantity_decimal_places, double price,
    std::uint8_t price_decimal_places, TimeInForce time_in_force,
    bool reduce_only, OrderSide side = OrderSide::kBuy) {
  core::OrderPlaceRequest request{
      .local_order_id = local_order_id,
      .price = price,
      .quantity = quantity,
      .side = side,
      .order_type = order_type,
      .time_in_force = time_in_force,
      .price_decimal_places = price_decimal_places,
      .quantity_decimal_places = quantity_decimal_places,
      .reduce_only = reduce_only,
  };
  core::SetOrderSymbol(&request, contract);
  return request;
}

TEST(GateOrderSignatureTest, MatchesLoginVector) {
  std::array<char, kGateSignatureHexSize> output{};

  const bool ok = GenerateGateApiSignatureHex("secret", "futures.login", "",
                                              1700000000, output);

  EXPECT_TRUE(ok);
  EXPECT_EQ(std::string_view(output.data(), output.size()),
            "f39035057b3528fc2c5aff4b9cfa9f43673c88d3ff823c5546860817320"
            "5809999a8b45d7ed898ebf49c15a4f6e5131de175ded143be5eeb58431f"
            "600e1d4085");
}

TEST(GateOrderSignatureTest, MatchesPrivateSubscribeVector) {
  std::array<char, kGateSignatureHexSize> output{};

  const bool ok = GenerateGateChannelSignatureHex(
      "secret", "futures.orders", "subscribe", 1700000000, output);

  EXPECT_TRUE(ok);
  EXPECT_EQ(std::string_view(output.data(), output.size()),
            "e40a0260c1d090a7b5f60f6171dfc3d9ca6a1b1dd6abfd8e92347bad5e13"
            "4af77fb03930eff122f7f72cdcb3c41898a16dbf1ee7089615f540c11b12c19"
            "b6d04");
}

TEST(GateOrderRequestEncoderTest,
     LoginRequestContainsChannelCredentialsTimestampAndRequestId) {
  std::array<char, kLoginRequestBufferSize> buffer{};
  const LoginRequestFields fields{
      .api_key = "key",
      .api_secret = "secret",
      .timestamp = 1700000000,
      .encoded_request_id = 72057594037927937ULL,
  };

  const EncodedTextRequest encoded = EncodeLoginRequest(fields, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_NE(encoded.text.find(R"("channel":"futures.login")"),
            std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("api_key":"key")"), std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("timestamp":"1700000000")"),
            std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("req_id":"72057594037927937")"),
            std::string_view::npos);
}

TEST(GateOrderRequestEncoderTest, OrderFeedbackSubscribeRequestWritesJson) {
  std::array<char, kOrderFeedbackSubscribeRequestBufferSize> buffer{};
  const OrderFeedbackSubscribeRequestFields fields{
      .api_key = "key",
      .api_secret = "secret",
      .timestamp = 1700000000,
      .login_uid = 14391412,
  };

  const EncodedTextRequest encoded =
      EncodeOrderFeedbackSubscribeRequest(fields, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_NE(encoded.text.find(R"("channel":"futures.orders")"),
            std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("event":"subscribe")"),
            std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("14391412")"), std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("!all")"), std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("KEY":"key")"), std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("SIGN":")"), std::string_view::npos);
}

TEST(GateOrderRequestEncoderTest, PlaceOrderWritesExactJson) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  const core::OrderPlaceRequest request =
      MakePlaceRequest(9, OrderType::kLimit, "BTC_USDT", 1.0, 0, 81000.0, 0,
                       TimeInForce::kGoodTillCancel, false);

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(
      request, 1700000001, 144115188075855873ULL, false, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"time":1700000001,"channel":"futures.order_place","event":"api","payload":{"req_id":"144115188075855873","req_param":{"contract":"BTC_USDT","size":1,"price":"81000","tif":"gtc","text":"t-9","reduce_only":false}}})");
}

TEST(GateOrderRequestEncoderTest, PlaceOrderWritesDecimalSizeJson) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  const core::OrderPlaceRequest request =
      MakePlaceRequest(9, OrderType::kLimit, "RAVE_USDT", 0.1, 1, 0.1234, 4,
                       TimeInForce::kImmediateOrCancel, false);

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(
      request, 1700000001, 144115188075855873ULL, false, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"time":1700000001,"channel":"futures.order_place","event":"api","payload":{"req_id":"144115188075855873","req_param":{"contract":"RAVE_USDT","size":0.1,"price":"0.1234","tif":"ioc","text":"t-9","reduce_only":false}}})");
}

TEST(GateOrderRequestEncoderTest, PlaceOrderQuotesSizeWhenRequested) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  const core::OrderPlaceRequest request =
      MakePlaceRequest(9, OrderType::kLimit, "RAVE_USDT", 0.1, 1, 0.1234, 4,
                       TimeInForce::kImmediateOrCancel, false);

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(
      request, 1700000001, 144115188075855873ULL, true, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"time":1700000001,"channel":"futures.order_place","event":"api","payload":{"req_id":"144115188075855873","req_param":{"contract":"RAVE_USDT","size":"0.1","price":"0.1234","tif":"ioc","text":"t-9","reduce_only":false}}})");
}

TEST(GateOrderRequestEncoderTest, MarketOrderReturnsUnsupportedOrderType) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  const core::OrderPlaceRequest request =
      MakePlaceRequest(9, OrderType::kMarket, "BTC_USDT", 1.0, 0, 81000.0, 0,
                       TimeInForce::kGoodTillCancel, false);

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(
      request, 1700000001, 144115188075855873ULL, false, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kUnsupportedOrderType);
  EXPECT_TRUE(encoded.text.empty());
}

TEST(GateOrderRequestEncoderTest, CancelByExchangeOrderIdWritesExactJson) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  const core::OrderCancelRequest request{.local_order_id = 11};

  const EncodedTextRequest encoded = EncodeCancelOrderRequest(
      request, 36028827892199865ULL, 1700000002, 216172782113783810ULL, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"time":1700000002,"channel":"futures.order_cancel","event":"api","payload":{"req_id":"216172782113783810","req_param":{"order_id":"36028827892199865"}}})");
}

TEST(GateOrderRequestEncoderTest, CancelFallbackUsesOrderText) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  const core::OrderCancelRequest request{.local_order_id = 11};

  const EncodedTextRequest encoded = EncodeCancelOrderRequest(
      request, 0, 1700000002, 216172782113783810ULL, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_NE(encoded.text.find(R"("order_id":"t-11")"), std::string_view::npos);
}

TEST(GateOrderRequestEncoderTest, SmallBufferReturnsBufferTooSmall) {
  std::array<char, 8> buffer{};
  const core::OrderPlaceRequest request =
      MakePlaceRequest(9, OrderType::kLimit, "BTC_USDT", 1.0, 0, 81000.0, 0,
                       TimeInForce::kGoodTillCancel, false);

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(
      request, 1700000001, 144115188075855873ULL, false, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kBufferTooSmall);
  EXPECT_TRUE(encoded.text.empty());
}

TEST(GateOrderRequestEncoderTest, MaximumBoundedPlaceFieldsFitDirectBuffer) {
  std::array<char, kPlaceOrderDirectFormatCapacity> buffer{};
  const core::OrderPlaceRequest request = MakePlaceRequest(
      std::numeric_limits<std::uint64_t>::max(), OrderType::kLimit,
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456", 999999.12345, 5, 999999999.123456, 6,
      TimeInForce::kImmediateOrCancel, true, OrderSide::kSell);

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(
      request, std::numeric_limits<std::int64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(), false, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_LT(encoded.text.size(), buffer.size());
}

TEST(GateOrderRequestEncoderTest,
     InvalidPlaceOrderTextReturnsInvalidOrderText) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  const core::OrderPlaceRequest request =
      MakePlaceRequest(0, OrderType::kLimit, "BTC_USDT", 1.0, 0, 81000.0, 0,
                       TimeInForce::kGoodTillCancel, false);

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(
      request, 1700000001, 144115188075855873ULL, false, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kInvalidOrderText);
  EXPECT_TRUE(encoded.text.empty());
}

TEST(GateOrderRequestEncoderTest,
     InvalidCancelFallbackReturnsInvalidOrderText) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  const core::OrderCancelRequest request{};

  const EncodedTextRequest encoded = EncodeCancelOrderRequest(
      request, 0, 1700000002, 216172782113783810ULL, buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kInvalidOrderText);
  EXPECT_TRUE(encoded.text.empty());
}

}  // namespace
}  // namespace aquila::gate
