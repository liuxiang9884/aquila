#include "exchange/bitget/trading/order_request_encoder.h"

#include <array>
#include <string_view>

#include <gtest/gtest.h>

namespace aquila::bitget {
namespace {

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
  const PlaceOrderEncodeFields fields{
      .encoded_request_id = 144115188075855873ULL,
      .local_order_id = 9,
      .order_type = OrderType::kLimit,
      .symbol = "BTCUSDT",
      .quantity_text = "0.001",
      .price_text = "100000.0",
      .side = OrderSide::kBuy,
      .time_in_force = TimeInForce::kImmediateOrCancel,
      .reduce_only = false,
  };

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(fields, buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"op":"trade","id":"144115188075855873","category":"usdt-futures","topic":"place-order","args":[{"symbol":"BTCUSDT","orderType":"limit","qty":"0.001","price":"100000.0","side":"buy","timeInForce":"ioc","reduceOnly":"NO","marginMode":"crossed","clientOid":"a-9"}]})");
}

TEST(BitgetOrderRequestEncoderTest, PlaceLimitGtcSellReduceOnlyWritesTokens) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  const PlaceOrderEncodeFields fields{
      .encoded_request_id = 144115188075855874ULL,
      .local_order_id = 10,
      .order_type = OrderType::kLimit,
      .symbol = "ETHUSDT",
      .quantity_text = "1.25",
      .price_text = "3200.5",
      .side = OrderSide::kSell,
      .time_in_force = TimeInForce::kGoodTillCancel,
      .reduce_only = true,
  };

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(fields, buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_NE(encoded.text.find(R"("side":"sell")"), std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("timeInForce":"gtc")"),
            std::string_view::npos);
  EXPECT_NE(encoded.text.find(R"("reduceOnly":"YES")"), std::string_view::npos);
}

TEST(BitgetOrderRequestEncoderTest, CancelWritesOrderIdAndClientOid) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  const CancelOrderEncodeFields fields{
      .encoded_request_id = 216172782113783810ULL,
      .local_order_id = 11,
      .exchange_order_id = 123456789,
  };

  const EncodedTextRequest encoded = EncodeCancelOrderRequest(fields, buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"op":"trade","id":"216172782113783810","category":"usdt-futures","topic":"cancel-order","args":[{"orderId":"123456789","clientOid":"a-11"}]})");
}

TEST(BitgetOrderRequestEncoderTest, CancelFallbackUsesOnlyClientOid) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  const CancelOrderEncodeFields fields{
      .encoded_request_id = 216172782113783811ULL,
      .local_order_id = 12,
      .exchange_order_id = 0,
  };

  const EncodedTextRequest encoded = EncodeCancelOrderRequest(fields, buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(
      encoded.text,
      R"({"op":"trade","id":"216172782113783811","category":"usdt-futures","topic":"cancel-order","args":[{"clientOid":"a-12"}]})");
}

TEST(BitgetOrderRequestEncoderTest, RejectsUnsupportedOrMissingPlaceFields) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  PlaceOrderEncodeFields fields{
      .encoded_request_id = 1,
      .local_order_id = 1,
      .order_type = OrderType::kMarket,
      .symbol = "BTCUSDT",
      .quantity_text = "1",
      .price_text = "1",
  };

  EXPECT_EQ(EncodePlaceOrderRequest(fields, buffer).status,
            OrderEncodeStatus::kUnsupportedOrderType);
  fields.order_type = OrderType::kLimit;
  fields.symbol = {};
  EXPECT_EQ(EncodePlaceOrderRequest(fields, buffer).status,
            OrderEncodeStatus::kInvalidSymbol);
  fields.symbol = "BTCUSDT";
  fields.quantity_text = {};
  EXPECT_EQ(EncodePlaceOrderRequest(fields, buffer).status,
            OrderEncodeStatus::kInvalidQuantityText);
  fields.quantity_text = "1";
  fields.price_text = {};
  EXPECT_EQ(EncodePlaceOrderRequest(fields, buffer).status,
            OrderEncodeStatus::kInvalidPriceText);
}

TEST(BitgetOrderRequestEncoderTest, RejectsInvalidLocalIdAndSmallBuffer) {
  std::array<char, kCancelOrderRequestBufferSize> cancel_buffer{};
  const CancelOrderEncodeFields cancel_fields{
      .encoded_request_id = 1,
      .local_order_id = 0,
  };
  EXPECT_EQ(EncodeCancelOrderRequest(cancel_fields, cancel_buffer).status,
            OrderEncodeStatus::kInvalidClientOid);

  std::array<char, 8> small_buffer{};
  const PlaceOrderEncodeFields place_fields{
      .encoded_request_id = 1,
      .local_order_id = 1,
      .order_type = OrderType::kLimit,
      .symbol = "BTCUSDT",
      .quantity_text = "1",
      .price_text = "1",
  };
  EXPECT_EQ(EncodePlaceOrderRequest(place_fields, small_buffer).status,
            OrderEncodeStatus::kBufferTooSmall);
}

}  // namespace
}  // namespace aquila::bitget
