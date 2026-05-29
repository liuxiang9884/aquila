#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_ID_ALLOCATOR_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_ID_ALLOCATOR_H_

#include <cstdint>

#include "core/trading/order_id.h"
#include "tools/gate/order_session_rtt_probe/sample_flow.h"

namespace aquila::tools::gate_order_session_rtt_probe {

class ProbeSampleIdAllocator {
 public:
  explicit ProbeSampleIdAllocator(std::uint8_t strategy_id) noexcept
      : ProbeSampleIdAllocator(strategy_id, /*first_strategy_order_id=*/1,
                               /*strategy_order_id_stride=*/4) {}

  ProbeSampleIdAllocator(std::uint8_t strategy_id,
                         std::uint64_t first_strategy_order_id,
                         std::uint64_t strategy_order_id_stride) noexcept
      : strategy_id_(strategy_id),
        next_strategy_order_id_(
            first_strategy_order_id == 0 ? 1 : first_strategy_order_id),
        strategy_order_id_stride_(
            strategy_order_id_stride < 4 ? 4 : strategy_order_id_stride) {}

  [[nodiscard]] ProbeSampleLocalIds Next() noexcept {
    const std::uint64_t first = next_strategy_order_id_;
    next_strategy_order_id_ += strategy_order_id_stride_;
    return ProbeSampleLocalIds{
        .gtc_local_order_id = LocalOrderIdCodec::Encode(strategy_id_, first),
        .ioc_local_order_id =
            LocalOrderIdCodec::Encode(strategy_id_, first + 1),
        .gtc_close_local_order_id =
            LocalOrderIdCodec::Encode(strategy_id_, first + 2),
        .ioc_close_local_order_id =
            LocalOrderIdCodec::Encode(strategy_id_, first + 3),
    };
  }

  [[nodiscard]] std::uint64_t next_strategy_order_id() const noexcept {
    return next_strategy_order_id_;
  }

 private:
  std::uint8_t strategy_id_{0};
  std::uint64_t next_strategy_order_id_{1};
  std::uint64_t strategy_order_id_stride_{4};
};

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_ID_ALLOCATOR_H_
