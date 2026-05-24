#ifndef AQUILA_EXCHANGE_GATE_TRADING_DECIMAL_SIZE_HEADER_H_
#define AQUILA_EXCHANGE_GATE_TRADING_DECIMAL_SIZE_HEADER_H_

#include <string>
#include <string_view>

#include "core/websocket/types.h"

namespace aquila::gate {

inline constexpr std::string_view kGateSizeDecimalHeaderName =
    "X-Gate-Size-Decimal";
inline constexpr std::string_view kGateSizeDecimalHeaderValue = "1";

inline void AddGateSizeDecimalHeader(websocket::ConnectionConfig& connection) {
  connection.extra_headers.push_back(websocket::HttpHeader{
      .name = std::string{kGateSizeDecimalHeaderName},
      .value = std::string{kGateSizeDecimalHeaderValue},
  });
}

[[nodiscard]] inline bool HasGateSizeDecimalHeader(
    const websocket::ConnectionConfig& connection) noexcept {
  for (const websocket::HttpHeader& header : connection.extra_headers) {
    if (header.name == kGateSizeDecimalHeaderName &&
        header.value == kGateSizeDecimalHeaderValue) {
      return true;
    }
  }
  return false;
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_DECIMAL_SIZE_HEADER_H_
