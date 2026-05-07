#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_REQUEST_ENCODER_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_REQUEST_ENCODER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "exchange/gate/trading/order_codecs.h"
#include "exchange/gate/trading/order_signature.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::gate {

inline constexpr std::size_t kLoginRequestBufferSize = 1024;
inline constexpr std::size_t kPlaceOrderRequestBufferSize = 1024;
inline constexpr std::size_t kCancelOrderRequestBufferSize = 512;

enum class OrderEncodeStatus : std::uint8_t {
  kOk,
  kBufferTooSmall,
  kSignatureFailed,
  kInvalidOrderText,
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

struct PlaceOrderEncodeFields {
  std::int64_t timestamp{0};
  std::uint64_t encoded_request_id{0};
  OrderWireFields wire{};
};

struct CancelOrderEncodeFields {
  std::int64_t timestamp{0};
  std::uint64_t encoded_request_id{0};
  std::int64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
};

namespace detail {

template <std::size_t N, typename... Args>
[[nodiscard]] EncodedTextRequest FormatJsonToBuffer(
    std::array<char, N>& buffer, fmt::format_string<Args...> format,
    Args&&... args) {
  const auto result = fmt::format_to_n(buffer.data(), buffer.size(), format,
                                       std::forward<Args>(args)...);
  if (result.size > buffer.size()) {
    return {};
  }
  return {.status = OrderEncodeStatus::kOk,
          .text = std::string_view(buffer.data(), result.size)};
}

}  // namespace detail

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
      R"({{"time":{},"channel":"futures.login","event":"api","payload":{{"api_key":"{}","signature":"{}","timestamp":"{}","req_id":"{}"}}}})",
      fields.timestamp, fields.api_key,
      std::string_view(signature.data(), signature.size()), fields.timestamp,
      fields.encoded_request_id);
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodePlaceOrderRequest(
    const PlaceOrderEncodeFields& fields,
    std::array<char, N>& buffer) noexcept {
  return detail::FormatJsonToBuffer(
      buffer,
      R"({{"time":{},"channel":"futures.order_place","event":"api","payload":{{"req_id":"{}","req_param":{{"contract":"{}","size":{},"price":"{}","tif":"{}","text":"{}","reduce_only":{}}}}}}})",
      fields.timestamp, fields.encoded_request_id, fields.wire.contract,
      fields.wire.signed_size, fields.wire.price_text, fields.wire.tif,
      fields.wire.text, fields.wire.reduce_only);
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodeCancelOrderRequest(
    const CancelOrderEncodeFields& fields,
    std::array<char, N>& buffer) noexcept {
  std::array<char, 32> fallback_order_id_buffer;
  std::string_view order_id{};
  if (fields.exchange_order_id != 0) {
    return detail::FormatJsonToBuffer(
        buffer,
        R"({{"time":{},"channel":"futures.order_cancel","event":"api","payload":{{"req_id":"{}","req_param":{{"order_id":"{}"}}}}}})",
        fields.timestamp, fields.encoded_request_id, fields.exchange_order_id);
  }

  order_id =
      OrderTextCodec::Format(fields.local_order_id, fallback_order_id_buffer);
  if (order_id.empty()) {
    return {.status = OrderEncodeStatus::kInvalidOrderText, .text = {}};
  }

  return detail::FormatJsonToBuffer(
      buffer,
      R"({{"time":{},"channel":"futures.order_cancel","event":"api","payload":{{"req_id":"{}","req_param":{{"order_id":"{}"}}}}}})",
      fields.timestamp, fields.encoded_request_id, order_id);
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_REQUEST_ENCODER_H_
