#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_CODECS_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_CODECS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "exchange/bitget/trading/order_types.h"

namespace aquila::bitget {

class RequestIdCodec {
 public:
  static constexpr std::uint64_t kSequenceMask = 0x00FFFFFFFFFFFFFFULL;

  [[nodiscard]] static constexpr std::uint64_t Encode(
      OrderRequestType type, std::uint64_t sequence) noexcept {
    return (static_cast<std::uint64_t>(type) << 56) |
           (sequence & kSequenceMask);
  }

  [[nodiscard]] static constexpr DecodedRequestId Decode(
      std::uint64_t encoded) noexcept {
    const auto type = static_cast<OrderRequestType>(encoded >> 56);
    const std::uint64_t sequence = encoded & kSequenceMask;
    if ((type != OrderRequestType::kPlaceOrder &&
         type != OrderRequestType::kCancelOrder) ||
        sequence == 0) {
      return {.ok = false,
              .type = OrderRequestType::kUnknown,
              .sequence = sequence};
    }
    return {.ok = true, .type = type, .sequence = sequence};
  }
};

class ClientOidCodec {
 public:
  static constexpr std::size_t kEncodedSize = 29;
  static constexpr std::size_t kLocalOrderIdEncodedSize = 13;

  [[nodiscard]] static bool ParseLocalOrderId(std::string_view text,
                                              std::uint64_t* output) noexcept {
    if (text.size() != kLocalOrderIdEncodedSize || output == nullptr) {
      return false;
    }
    std::uint64_t value = 0;
    for (const char byte : text) {
      std::uint64_t digit = 0;
      if (byte >= '0' && byte <= '9') {
        digit = static_cast<std::uint64_t>(byte - '0');
      } else if (byte >= 'A' && byte <= 'Z') {
        digit = static_cast<std::uint64_t>(byte - 'A' + 10);
      } else {
        return false;
      }
      if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 36) {
        return false;
      }
      value = value * 36 + digit;
    }
    if (value == 0) {
      return false;
    }
    *output = value;
    return true;
  }

  template <std::size_t N>
  [[nodiscard]] static std::string_view Format(
      const ClientOidRunNamespace& run_namespace, std::uint64_t local_order_id,
      std::array<char, N>& output) noexcept {
    if constexpr (N < kEncodedSize) {
      return {};
    }
    if (!run_namespace.IsConfigured() || local_order_id == 0) {
      return {};
    }
    output[0] = 'a';
    output[1] = '1';
    output[2] = '-';
    const std::string_view namespace_text = run_namespace.View();
    for (std::size_t i = 0; i < namespace_text.size(); ++i) {
      output[3 + i] = namespace_text[i];
    }
    output[15] = '-';
    constexpr std::string_view kBase36Alphabet =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (std::size_t index = kEncodedSize; index > 16; --index) {
      output[index - 1] = kBase36Alphabet[local_order_id % 36];
      local_order_id /= 36;
    }
    if (local_order_id != 0) {
      return {};
    }
    return std::string_view(output.data(), kEncodedSize);
  }

  [[nodiscard]] static ParsedClientOid Parse(std::string_view text) noexcept {
    if (text.size() != kEncodedSize || text[0] != 'a' || text[1] != '1' ||
        text[2] != '-' || text[15] != '-') {
      return {};
    }
    const std::optional<ClientOidRunNamespace> run_namespace =
        ClientOidRunNamespace::Parse(
            text.substr(3, kClientOidRunNamespaceSize));
    if (!run_namespace.has_value()) {
      return {};
    }
    std::uint64_t local_order_id = 0;
    if (!ParseLocalOrderId(text.substr(16), &local_order_id)) {
      return {};
    }
    return {.ok = true,
            .run_namespace = *run_namespace,
            .local_order_id = local_order_id};
  }
};

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_CODECS_H_
