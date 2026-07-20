#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_REQUEST_ENCODER_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_REQUEST_ENCODER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <fmt/compile.h>
#include <fmt/format.h>

#include "core/trading/order_decimal.h"
#include "core/trading/order_types.h"
#include "exchange/bitget/trading/order_codecs.h"
#include "exchange/bitget/trading/order_signature.h"

namespace aquila::bitget {

inline constexpr std::size_t kLoginRequestBufferSize = 512;
inline constexpr std::size_t kPlaceOrderRequestBufferSize = 1024;
inline constexpr std::size_t kCancelOrderRequestBufferSize = 512;
inline constexpr std::size_t kPlaceOrderDirectFormatCapacity = 512;
static_assert(kPlaceOrderRequestBufferSize >= kPlaceOrderDirectFormatCapacity);

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

namespace detail {

template <std::size_t N, typename Format, typename... Args>
[[nodiscard]] EncodedTextRequest FormatJsonToBuffer(std::array<char, N>& buffer,
                                                    const Format& format,
                                                    Args&&... args) noexcept {
  const auto result = fmt::format_to_n(buffer.data(), buffer.size(), format,
                                       std::forward<Args>(args)...);
  if (result.size > buffer.size()) {
    return {};
  }
  return {.status = OrderEncodeStatus::kOk,
          .text = std::string_view(buffer.data(), result.size)};
}

template <std::size_t N, typename Format, typename... Args>
[[nodiscard]] EncodedTextRequest FormatPlaceJsonToBuffer(
    std::array<char, N>& buffer, const Format& format,
    Args&&... args) noexcept {
  if constexpr (N < kPlaceOrderDirectFormatCapacity) {
    return FormatJsonToBuffer(buffer, format, std::forward<Args>(args)...);
  } else {
    // OrderPlaceRequest bounds the complete place payload below 512 bytes.
    char* const end =
        fmt::format_to(buffer.data(), format, std::forward<Args>(args)...);
    return {.status = OrderEncodeStatus::kOk,
            .text = std::string_view(
                buffer.data(), static_cast<std::size_t>(end - buffer.data()))};
  }
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
      FMT_COMPILE(
          R"({{"op":"login","args":[{{"apiKey":"{}","passphrase":"{}","timestamp":"{}","sign":"{}"}}]}})"),
      fields.api_key, fields.passphrase, fields.timestamp_seconds,
      std::string_view(signature.data(), signature.size()));
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodePlaceOrderRequest(
    const core::OrderPlaceRequest& request, std::uint64_t encoded_request_id,
    std::array<char, N>& buffer) noexcept {
  if (request.order_type != OrderType::kLimit) {
    return {.status = OrderEncodeStatus::kUnsupportedOrderType};
  }
  if (!detail::IsSafeJsonString(request.SymbolView(), 64)) {
    return {.status = OrderEncodeStatus::kInvalidSymbol};
  }
  std::array<char, 32> client_oid_buffer{};
  const std::string_view client_oid =
      ClientOidCodec::Format(request.local_order_id, client_oid_buffer);
  if (client_oid.empty()) {
    return {.status = OrderEncodeStatus::kInvalidClientOid};
  }
  std::array<char, 32> quantity_text_buffer;
  std::array<char, 32> price_text_buffer;
  const std::string_view quantity_text = core::FormatDecimalValue(
      request.quantity, request.quantity_decimal_places, quantity_text_buffer);
  const std::string_view price_text = core::FormatDecimalValue(
      request.price, request.price_decimal_places, price_text_buffer);
  return detail::FormatPlaceJsonToBuffer(
      buffer,
      FMT_COMPILE(
          R"({{"op":"trade","id":"{}","category":"usdt-futures","topic":"place-order","args":[{{"symbol":"{}","orderType":"limit","qty":"{}","price":"{}","side":"{}","timeInForce":"{}","reduceOnly":"{}","marginMode":"crossed","clientOid":"{}"}}]}})"),
      encoded_request_id, request.SymbolView(), quantity_text, price_text,
      BitgetOrderSideToken(request.side),
      BitgetTimeInForceToken(request.time_in_force),
      request.reduce_only ? "YES" : "NO", client_oid);
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodeCancelOrderRequest(
    const core::OrderCancelRequest& request, std::uint64_t exchange_order_id,
    std::uint64_t encoded_request_id, std::array<char, N>& buffer) noexcept {
  std::array<char, 32> client_oid_buffer{};
  const std::string_view client_oid =
      ClientOidCodec::Format(request.local_order_id, client_oid_buffer);
  if (client_oid.empty()) {
    return {.status = OrderEncodeStatus::kInvalidClientOid};
  }
  if (exchange_order_id != 0) {
    return detail::FormatJsonToBuffer(
        buffer,
        FMT_COMPILE(
            R"({{"op":"trade","id":"{}","category":"usdt-futures","topic":"cancel-order","args":[{{"orderId":"{}","clientOid":"{}"}}]}})"),
        encoded_request_id, exchange_order_id, client_oid);
  }
  return detail::FormatJsonToBuffer(
      buffer,
      FMT_COMPILE(
          R"({{"op":"trade","id":"{}","category":"usdt-futures","topic":"cancel-order","args":[{{"clientOid":"{}"}}]}})"),
      encoded_request_id, client_oid);
}

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_REQUEST_ENCODER_H_
