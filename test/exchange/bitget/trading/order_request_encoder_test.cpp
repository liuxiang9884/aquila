#include "exchange/bitget/trading/order_request_encoder.h"

#include <array>
#include <limits>
#include <string_view>

#include <gtest/gtest.h>

namespace aquila::bitget {
namespace {

core::OrderPlaceRequest MakePlaceRequest(
    std::uint64_t local_order_id, OrderType order_type, std::string_view symbol,
    double quantity, std::uint8_t quantity_decimal_places, double price,
    std::uint8_t price_decimal_places, OrderSide side,
    TimeInForce time_in_force, bool reduce_only) {
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
  core::SetOrderSymbol(&request, symbol);
  return request;
}

TEST(BitgetOrderRequestEncoderTest, GeneratesDeterministicLoginSignature) {
  std::array<char, kBitgetLoginSignatureBase64Size> signature{};

  ASSERT_TRUE(
      GenerateBitgetLoginSignatureBase64("secret", 1700000000, signature));
  EXPECT_EQ(std::string_view(signature.data(), signature.size()),
            "asp8h2LSGzNFWF9BshQJj0WiZA5uDIWsAk9FCfz2Ilk=");
}

TEST(BitgetOrderRequestEncoderTest, LoginWritesUnixSecondsAndCredentials) {
  std::array<char, kLoginRequestBufferSize> buffer{};
  const LoginRequestFields fields{
      .api_key = "key",
      .api_secret = "secret",
      .passphrase = "phrase",
      .timestamp_seconds = 1700000000,
  };

  const EncodedTextRequest encoded = EncodeLoginRequest(fields, buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"op":"login","args":[{"apiKey":"key","passphrase":"phrase","timestamp":"1700000000","sign":"asp8h2LSGzNFWF9BshQJj0WiZA5uDIWsAk9FCfz2Ilk="}]})");
}

TEST(BitgetOrderRequestEncoderTest, PlaceLimitIocBuyWritesExactJson) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  const core::OrderPlaceRequest request =
      MakePlaceRequest(9, OrderType::kLimit, "BTCUSDT", 0.001, 3, 100000.0, 1,
                       OrderSide::kBuy, TimeInForce::kImmediateOrCancel, false);

  const EncodedTextRequest encoded =
      EncodePlaceOrderRequest(request, 144115188075855873ULL, buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"op":"trade","id":"144115188075855873","category":"usdt-futures","topic":"place-order","args":[{"symbol":"BTCUSDT","orderType":"limit","qty":"0.001","price":"100000.0","side":"buy","timeInForce":"ioc","reduceOnly":"NO","marginMode":"crossed","clientOid":"a-9"}]})");
}

TEST(BitgetOrderRequestEncoderTest, PlaceLimitGtcSellReduceOnlyWritesTokens) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  const core::OrderPlaceRequest request =
      MakePlaceRequest(10, OrderType::kLimit, "ETHUSDT", 1.25, 2, 3200.5, 1,
                       OrderSide::kSell, TimeInForce::kGoodTillCancel, true);

  const EncodedTextRequest encoded =
      EncodePlaceOrderRequest(request, 144115188075855874ULL, buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_NE(encoded.text.find(R"("side":"sell")"), std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("timeInForce":"gtc")"),
            std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("reduceOnly":"YES")"), std::string_view::npos);
}

TEST(BitgetOrderRequestEncoderTest, CancelWritesOrderIdAndClientOid) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  const core::OrderCancelRequest request{.local_order_id = 11};

  const EncodedTextRequest encoded = EncodeCancelOrderRequest(
      request, 123456789, 216172782113783810ULL, buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"op":"trade","id":"216172782113783810","category":"usdt-futures","topic":"cancel-order","args":[{"orderId":"123456789","clientOid":"a-11"}]})");
}

TEST(BitgetOrderRequestEncoderTest, CancelFallbackUsesOnlyClientOid) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  const core::OrderCancelRequest request{.local_order_id = 12};

  const EncodedTextRequest encoded =
      EncodeCancelOrderRequest(request, 0, 216172782113783811ULL, buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"op":"trade","id":"216172782113783811","category":"usdt-futures","topic":"cancel-order","args":[{"clientOid":"a-12"}]})");
}

TEST(BitgetOrderRequestEncoderTest, RejectsUnsupportedOrMissingPlaceFields) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  core::OrderPlaceRequest request =
      MakePlaceRequest(1, OrderType::kMarket, "BTCUSDT", 1.0, 0, 1.0, 0,
                       OrderSide::kBuy, TimeInForce::kGoodTillCancel, false);

  EXPECT_EQ(EncodePlaceOrderRequest(request, 1, buffer).status,
            OrderEncodeStatus::kUnsupportedOrderType);
  request.order_type = OrderType::kLimit;
  request.symbol_size = 0;
  EXPECT_EQ(EncodePlaceOrderRequest(request, 1, buffer).status,
            OrderEncodeStatus::kInvalidSymbol);
}

TEST(BitgetOrderRequestEncoderTest, RejectsInvalidLocalIdAndSmallBuffer) {
  std::array<char, kCancelOrderRequestBufferSize> cancel_buffer{};
  const core::OrderCancelRequest cancel_request{};
  EXPECT_EQ(
      EncodeCancelOrderRequest(cancel_request, 0, 1, cancel_buffer).status,
      OrderEncodeStatus::kInvalidClientOid);

  std::array<char, 8> small_buffer{};
  const core::OrderPlaceRequest place_request =
      MakePlaceRequest(1, OrderType::kLimit, "BTCUSDT", 1.0, 0, 1.0, 0,
                       OrderSide::kBuy, TimeInForce::kGoodTillCancel, false);
  EXPECT_EQ(EncodePlaceOrderRequest(place_request, 1, small_buffer).status,
            OrderEncodeStatus::kBufferTooSmall);
}

TEST(BitgetOrderRequestEncoderTest, MaximumBoundedPlaceFieldsFitDirectBuffer) {
  std::array<char, kPlaceOrderDirectFormatCapacity> buffer{};
  const core::OrderPlaceRequest request = MakePlaceRequest(
      std::numeric_limits<std::uint64_t>::max(), OrderType::kLimit,
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456", 999999.12345, 5, 999999999.123456, 6,
      OrderSide::kSell, TimeInForce::kImmediateOrCancel, true);

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(
      request, std::numeric_limits<std::uint64_t>::max(), buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_LT(encoded.text.size(), buffer.size());
}

}  // namespace
}  // namespace aquila::bitget
