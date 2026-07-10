#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SAMPLE_ID_ALLOCATOR_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SAMPLE_ID_ALLOCATOR_H_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "core/trading/order_id.h"

namespace aquila::tools::bitget_order_session_rtt_probe {

struct ProbeSampleLocalIds {
  std::uint64_t ioc_local_order_id{0};
  std::uint64_t close_local_order_id{0};
};

class ProbeSampleIdAllocator {
 public:
  explicit ProbeSampleIdAllocator(std::uint8_t strategy_id) noexcept
      : ProbeSampleIdAllocator(strategy_id, 1, 2) {}

  ProbeSampleIdAllocator(std::uint8_t strategy_id,
                         std::uint64_t first_strategy_order_id,
                         std::uint64_t strategy_order_id_stride) noexcept
      : strategy_id_(strategy_id),
        next_strategy_order_id_(
            first_strategy_order_id == 0 ? 1 : first_strategy_order_id),
        strategy_order_id_stride_(
            strategy_order_id_stride < 2 ? 2 : strategy_order_id_stride) {}

  [[nodiscard]] ProbeSampleLocalIds Next() noexcept {
    const std::uint64_t first = next_strategy_order_id_;
    next_strategy_order_id_ += strategy_order_id_stride_;
    return ProbeSampleLocalIds{
        .ioc_local_order_id = LocalOrderIdCodec::Encode(strategy_id_, first),
        .close_local_order_id =
            LocalOrderIdCodec::Encode(strategy_id_, first + 1),
    };
  }

 private:
  std::uint8_t strategy_id_{0};
  std::uint64_t next_strategy_order_id_{1};
  std::uint64_t strategy_order_id_stride_{2};
};

[[nodiscard]] inline std::optional<std::size_t> SessionIndexForLocalOrderId(
    std::uint64_t local_order_id, std::size_t session_count) noexcept {
  if (local_order_id == 0 || session_count == 0) {
    return std::nullopt;
  }
  const std::uint64_t strategy_order_id =
      LocalOrderIdCodec::StrategyOrderId(local_order_id);
  if (strategy_order_id == 0) {
    return std::nullopt;
  }
  const std::uint64_t sample_first_order_id =
      strategy_order_id - ((strategy_order_id - 1) % 2);
  const std::uint64_t sample_index = (sample_first_order_id - 1) / 2;
  return static_cast<std::size_t>(sample_index % session_count);
}

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SAMPLE_ID_ALLOCATOR_H_
