#ifndef AQUILA_CORE_TRADING_ORDER_ID_H_
#define AQUILA_CORE_TRADING_ORDER_ID_H_

#include <cstdint>

namespace aquila {

class LocalOrderIdCodec {
 public:
  static constexpr std::uint64_t kStrategyOrderIdMask = 0x00FFFFFFFFFFFFFFULL;

  [[nodiscard]] static constexpr std::uint64_t Encode(
      std::uint8_t strategy_id, std::uint64_t strategy_order_id) noexcept {
    return (static_cast<std::uint64_t>(strategy_id) << 56) |
           (strategy_order_id & kStrategyOrderIdMask);
  }

  [[nodiscard]] static constexpr std::uint8_t StrategyId(
      std::uint64_t local_order_id) noexcept {
    return static_cast<std::uint8_t>(local_order_id >> 56);
  }

  [[nodiscard]] static constexpr std::uint64_t StrategyOrderId(
      std::uint64_t local_order_id) noexcept {
    return local_order_id & kStrategyOrderIdMask;
  }
};

}  // namespace aquila

#endif  // AQUILA_CORE_TRADING_ORDER_ID_H_
