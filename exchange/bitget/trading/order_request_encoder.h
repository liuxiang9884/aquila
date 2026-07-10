#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_REQUEST_ENCODER_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_REQUEST_ENCODER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "core/common/types.h"
#include "exchange/bitget/trading/order_codecs.h"
#include "exchange/bitget/trading/order_signature.h"

namespace aquila::bitget {

inline constexpr std::size_t kLoginRequestBufferSize = 512;
inline constexpr std::size_t kPlaceOrderRequestBufferSize = 1024;
inline constexpr std::size_t kCancelOrderRequestBufferSize = 512;

enum class OrderEncodeStatus : std::uint8_t {
  kOk,
  kBufferTooSmall,
  kSignatureFailed,
  kInvalidCredentials,
  kInvalidClientOid,
  kInvalidSymbol,
  kInvalidQuantityText,
  kInvalidPriceText,
  kUnsupportedOrderType,
};

struct EncodedTextRequest {
  OrderEncodeStatus status{OrderEncodeStatus::kBufferTooSmall};
  std::string_view text{};
};

struct LoginRequestFields {
  std::string_view api_key{};
  std::string_view api_secret{};
  std::string_view passphrase{};
  std::int64_t timestamp_seconds{0};
};

struct PlaceOrderEncodeFields {
  std::uint64_t encoded_request_id{0};
  std::uint64_t local_order_id{0};
  OrderType order_type{OrderType::kLimit};
  std::string_view symbol{};
  std::string_view quantity_text{};
  std::string_view price_text{};
  OrderSide side{OrderSide::kBuy};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  bool reduce_only{false};
};

struct CancelOrderEncodeFields {
  std::uint64_t encoded_request_id{0};
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
};

namespace detail {

template <std::size_t N, typename... Args>
[[nodiscard]] EncodedTextRequest FormatJsonToBuffer(
    std::array<char, N>& buffer, fmt::format_string<Args...> format,
    Args&&... args) noexcept {
  const auto result = fmt::format_to_n(buffer.data(), buffer.size(), format,
                                       std::forward<Args>(args)...);
  if (result.size > buffer.size()) {
    return {};
  }
  return {.status = OrderEncodeStatus::kOk,
          .text = std::string_view(buffer.data(), result.size)};
}

[[nodiscard]] inline bool IsSafeJsonString(std::string_view value,
                                           std::size_t max_size) noexcept {
  if (value.empty() || value.size() > max_size) {
    return false;
  }
  for (const unsigned char byte : value) {
    if (byte < 0x20 || byte == '"' || byte == '\\') {
      return false;
    }
  }
  return true;
}

}  // namespace detail

[[nodiscard]] constexpr std::string_view BitgetOrderSideToken(
    OrderSide side) noexcept {
  return side == OrderSide::kBuy ? "buy" : "sell";
}

[[nodiscard]] constexpr std::string_view BitgetTimeInForceToken(
    TimeInForce time_in_force) noexcept {
  return time_in_force == TimeInForce::kImmediateOrCancel ? "ioc" : "gtc";
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodeLoginRequest(
    const LoginRequestFields& fields, std::array<char, N>& buffer) noexcept {
  if (!detail::IsSafeJsonString(fields.api_key, 128) ||
      !detail::IsSafeJsonString(fields.passphrase, 128)) {
    return {.status = OrderEncodeStatus::kInvalidCredentials};
  }
  std::array<char, kBitgetLoginSignatureBase64Size> signature{};
  if (!GenerateBitgetLoginSignatureBase64(
          fields.api_secret, fields.timestamp_seconds, signature)) {
    return {.status = OrderEncodeStatus::kSignatureFailed};
  }
  return detail::FormatJsonToBuffer(
      buffer,
      R"({{"op":"login","args":[{{"apiKey":"{}","passphrase":"{}","timestamp":"{}","sign":"{}"}}]}})",
      fields.api_key, fields.passphrase, fields.timestamp_seconds,
      std::string_view(signature.data(), signature.size()));
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodePlaceOrderRequest(
    const PlaceOrderEncodeFields& fields,
    std::array<char, N>& buffer) noexcept {
  if (fields.order_type != OrderType::kLimit) {
    return {.status = OrderEncodeStatus::kUnsupportedOrderType};
  }
  if (!detail::IsSafeJsonString(fields.symbol, 64)) {
    return {.status = OrderEncodeStatus::kInvalidSymbol};
  }
  if (!detail::IsSafeJsonString(fields.quantity_text, 64)) {
    return {.status = OrderEncodeStatus::kInvalidQuantityText};
  }
  if (!detail::IsSafeJsonString(fields.price_text, 64)) {
    return {.status = OrderEncodeStatus::kInvalidPriceText};
  }
  std::array<char, 32> client_oid_buffer{};
  const std::string_view client_oid =
      ClientOidCodec::Format(fields.local_order_id, client_oid_buffer);
  if (client_oid.empty()) {
    return {.status = OrderEncodeStatus::kInvalidClientOid};
  }
  return detail::FormatJsonToBuffer(
      buffer,
      R"({{"op":"trade","id":"{}","category":"usdt-futures","topic":"place-order","args":[{{"symbol":"{}","orderType":"limit","qty":"{}","price":"{}","side":"{}","timeInForce":"{}","reduceOnly":"{}","marginMode":"crossed","clientOid":"{}"}}]}})",
      fields.encoded_request_id, fields.symbol, fields.quantity_text,
      fields.price_text, BitgetOrderSideToken(fields.side),
      BitgetTimeInForceToken(fields.time_in_force),
      fields.reduce_only ? "YES" : "NO", client_oid);
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodeCancelOrderRequest(
    const CancelOrderEncodeFields& fields,
    std::array<char, N>& buffer) noexcept {
  std::array<char, 32> client_oid_buffer{};
  const std::string_view client_oid =
      ClientOidCodec::Format(fields.local_order_id, client_oid_buffer);
  if (client_oid.empty()) {
    return {.status = OrderEncodeStatus::kInvalidClientOid};
  }
  if (fields.exchange_order_id != 0) {
    return detail::FormatJsonToBuffer(
        buffer,
        R"({{"op":"trade","id":"{}","category":"usdt-futures","topic":"cancel-order","args":[{{"orderId":"{}","clientOid":"{}"}}]}})",
        fields.encoded_request_id, fields.exchange_order_id, client_oid);
  }
  return detail::FormatJsonToBuffer(
      buffer,
      R"({{"op":"trade","id":"{}","category":"usdt-futures","topic":"cancel-order","args":[{{"clientOid":"{}"}}]}})",
      fields.encoded_request_id, client_oid);
}

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_REQUEST_ENCODER_H_
