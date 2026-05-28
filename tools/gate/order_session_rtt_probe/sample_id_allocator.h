#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_ID_ALLOCATOR_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_ID_ALLOCATOR_H_

#include <cstdint>

#include "core/trading/order_id.h"
#include "tools/gate/order_session_rtt_probe/sample_flow.h"

namespace aquila::tools::gate_order_session_rtt_probe {

class ProbeSampleIdAllocator {
 public:
  explicit ProbeSampleIdAllocator(std::uint8_t strategy_id) noexcept
      : strategy_id_(strategy_id) {}

  [[nodiscard]] ProbeSampleLocalIds Next() noexcept {
    const std::uint64_t first = next_strategy_order_id_;
    next_strategy_order_id_ += 4;
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
};

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_ID_ALLOCATOR_H_
