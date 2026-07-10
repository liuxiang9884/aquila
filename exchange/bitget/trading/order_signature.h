#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SIGNATURE_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SIGNATURE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aquila::bitget {

inline constexpr std::size_t kBitgetLoginSignatureBase64Size = 44;

[[nodiscard]] bool GenerateBitgetLoginSignatureBase64(
    std::string_view api_secret, std::int64_t timestamp_seconds,
    std::array<char, kBitgetLoginSignatureBase64Size>& output) noexcept;

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_SIGNATURE_H_
