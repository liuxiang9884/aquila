#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_CODECS_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_CODECS_H_

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

#include <fmt/format.h>

#include "exchange/gate/trading/order_types.h"

namespace aquila::gate {

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
    const auto type_value = static_cast<std::uint8_t>(encoded >> 56);
    const std::uint64_t sequence = encoded & kSequenceMask;
    switch (static_cast<OrderRequestType>(type_value)) {
      case OrderRequestType::kLogin:
      case OrderRequestType::kPlaceOrder:
      case OrderRequestType::kCancelOrder:
        return {.ok = true,
                .type = static_cast<OrderRequestType>(type_value),
                .sequence = sequence};
      case OrderRequestType::kUnknown:
        return {.ok = false,
                .type = OrderRequestType::kUnknown,
                .sequence = sequence};
    }
    return {
        .ok = false, .type = OrderRequestType::kUnknown, .sequence = sequence};
  }
};

class OrderTextCodec {
 public:
  template <std::size_t N>
  [[nodiscard]] static std::string_view Format(
      std::uint64_t local_order_id, std::array<char, N>& output) noexcept {
    if (local_order_id == 0) {
      return {};
    }

    const auto result =
        fmt::format_to_n(output.data(), output.size(), "t-{}", local_order_id);
    if (result.size > output.size()) {
      return {};
    }
    return std::string_view(output.data(), result.size);
  }

  [[nodiscard]] static ParsedOrderText Parse(std::string_view text) noexcept {
    if (text.size() <= 2 || text[0] != 't' || text[1] != '-') {
      return {};
    }

    std::uint64_t local_order_id = 0;
    const char* const first = text.data() + 2;
    const char* const last = text.data() + text.size();
    const auto result = std::from_chars(first, last, local_order_id);
    if (result.ec != std::errc{} || result.ptr != last || local_order_id == 0) {
      return {};
    }

    return {.ok = true, .local_order_id = local_order_id};
  }
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_CODECS_H_
