#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_SIGNATURE_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_SIGNATURE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aquila::gate {

inline constexpr std::size_t kGateSignatureHexSize = 128;

[[nodiscard]] bool GenerateGateApiSignatureHex(
    std::string_view api_secret, std::string_view channel,
    std::string_view request_param, std::int64_t timestamp,
    std::array<char, kGateSignatureHexSize>& output) noexcept;

[[nodiscard]] bool GenerateGateChannelSignatureHex(
    std::string_view api_secret, std::string_view channel,
    std::string_view event, std::int64_t timestamp,
    std::array<char, kGateSignatureHexSize>& output) noexcept;

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_SIGNATURE_H_
