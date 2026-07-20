#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_REQUEST_ENCODER_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_REQUEST_ENCODER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <fmt/compile.h>
#include <fmt/format.h>

#include "core/trading/order_decimal.h"
#include "core/trading/order_types.h"
#include "exchange/gate/trading/order_codecs.h"
#include "exchange/gate/trading/order_signature.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::gate {

inline constexpr std::size_t kLoginRequestBufferSize = 1024;
inline constexpr std::size_t kPlaceOrderRequestBufferSize = 1024;
inline constexpr std::size_t kCancelOrderRequestBufferSize = 512;
inline constexpr std::size_t kOrderFeedbackSubscribeRequestBufferSize = 1024;
inline constexpr std::size_t kPlaceOrderDirectFormatCapacity = 512;
static_assert(kPlaceOrderRequestBufferSize >= kPlaceOrderDirectFormatCapacity);

enum class OrderEncodeStatus : std::uint8_t {
  kOk,
  kBufferTooSmall,
  kSignatureFailed,
  kInvalidOrderText,
  kInvalidQuantityText,
  kUnsupportedOrderType,
};

struct EncodedTextRequest {
  OrderEncodeStatus status{OrderEncodeStatus::kBufferTooSmall};
  std::string_view text{};
};

struct LoginRequestFields {
  std::string_view api_key{};
  std::string_view api_secret{};
  std::int64_t timestamp{0};
  std::uint64_t encoded_request_id{0};
};

struct OrderFeedbackSubscribeRequestFields {
  std::string_view api_key{};
  std::string_view api_secret{};
  std::int64_t timestamp{0};
  std::uint64_t login_uid{0};
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

}  // namespace detail

[[nodiscard]] constexpr std::string_view GateTimeInForceToken(
    TimeInForce time_in_force) noexcept {
  switch (time_in_force) {
    case TimeInForce::kGoodTillCancel:
      return "gtc";
    case TimeInForce::kImmediateOrCancel:
      return "ioc";
  }
  return "gtc";
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodeLoginRequest(
    const LoginRequestFields& fields, std::array<char, N>& buffer) noexcept {
  std::array<char, kGateSignatureHexSize> signature;
  if (!GenerateGateApiSignatureHex(fields.api_secret, "futures.login", "",
                                   fields.timestamp, signature)) {
    return {.status = OrderEncodeStatus::kSignatureFailed, .text = {}};
  }

  return detail::FormatJsonToBuffer(
      buffer,
      FMT_COMPILE(
          R"({{"time":{},"channel":"futures.login","event":"api","payload":{{"api_key":"{}","signature":"{}","timestamp":"{}","req_id":"{}"}}}})"),
      fields.timestamp, fields.api_key,
      std::string_view(signature.data(), signature.size()), fields.timestamp,
      fields.encoded_request_id);
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodePlaceOrderRequest(
    const core::OrderPlaceRequest& request, std::int64_t timestamp,
    std::uint64_t encoded_request_id, bool quote_size,
    std::array<char, N>& buffer) noexcept {
  if (request.order_type != OrderType::kLimit) {
    return {.status = OrderEncodeStatus::kUnsupportedOrderType, .text = {}};
  }

  std::array<char, 32> text_buffer;
  const std::string_view text =
      OrderTextCodec::Format(request.local_order_id, text_buffer);
  if (text.empty()) {
    return {.status = OrderEncodeStatus::kInvalidOrderText, .text = {}};
  }

  const double signed_quantity =
      request.side == OrderSide::kBuy ? request.quantity : -request.quantity;
  std::array<char, 32> quantity_text_buffer;
  std::array<char, 32> price_text_buffer;
  const std::string_view quantity_text = core::FormatDecimalValue(
      signed_quantity, request.quantity_decimal_places, quantity_text_buffer);
  const std::string_view price_text = core::FormatDecimalValue(
      request.price, request.price_decimal_places, price_text_buffer);
  if (quote_size) {
    return detail::FormatPlaceJsonToBuffer(
        buffer,
        FMT_COMPILE(
            R"({{"time":{},"channel":"futures.order_place","event":"api","payload":{{"req_id":"{}","req_param":{{"contract":"{}","size":"{}","price":"{}","tif":"{}","text":"{}","reduce_only":{}}}}}}})"),
        timestamp, encoded_request_id, request.SymbolView(), quantity_text,
        price_text, GateTimeInForceToken(request.time_in_force), text,
        request.reduce_only);
  }
  return detail::FormatPlaceJsonToBuffer(
      buffer,
      FMT_COMPILE(
          R"({{"time":{},"channel":"futures.order_place","event":"api","payload":{{"req_id":"{}","req_param":{{"contract":"{}","size":{},"price":"{}","tif":"{}","text":"{}","reduce_only":{}}}}}}})"),
      timestamp, encoded_request_id, request.SymbolView(), quantity_text,
      price_text, GateTimeInForceToken(request.time_in_force), text,
      request.reduce_only);
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodeCancelOrderRequest(
    const core::OrderCancelRequest& request, std::uint64_t exchange_order_id,
    std::int64_t timestamp, std::uint64_t encoded_request_id,
    std::array<char, N>& buffer) noexcept {
  std::array<char, 32> fallback_order_id_buffer;
  std::string_view order_id{};
  if (exchange_order_id != 0) {
    return detail::FormatJsonToBuffer(
        buffer,
        FMT_COMPILE(
            R"({{"time":{},"channel":"futures.order_cancel","event":"api","payload":{{"req_id":"{}","req_param":{{"order_id":"{}"}}}}}})"),
        timestamp, encoded_request_id, exchange_order_id);
  }

  order_id =
      OrderTextCodec::Format(request.local_order_id, fallback_order_id_buffer);
  if (order_id.empty()) {
    return {.status = OrderEncodeStatus::kInvalidOrderText, .text = {}};
  }

  return detail::FormatJsonToBuffer(
      buffer,
      FMT_COMPILE(
          R"({{"time":{},"channel":"futures.order_cancel","event":"api","payload":{{"req_id":"{}","req_param":{{"order_id":"{}"}}}}}})"),
      timestamp, encoded_request_id, order_id);
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodeOrderFeedbackSubscribeRequest(
    const OrderFeedbackSubscribeRequestFields& fields,
    std::array<char, N>& buffer) noexcept {
  std::array<char, kGateSignatureHexSize> signature;
  if (!GenerateGateChannelSignatureHex(fields.api_secret, "futures.orders",
                                       "subscribe", fields.timestamp,
                                       signature)) {
    return {.status = OrderEncodeStatus::kSignatureFailed, .text = {}};
  }

  return detail::FormatJsonToBuffer(
      buffer,
      FMT_COMPILE(
          R"({{"time":{},"channel":"futures.orders","event":"subscribe","payload":["{}","!all"],"auth":{{"method":"api_key","KEY":"{}","SIGN":"{}"}}}})"),
      fields.timestamp, fields.login_uid, fields.api_key,
      std::string_view(signature.data(), signature.size()));
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_REQUEST_ENCODER_H_
