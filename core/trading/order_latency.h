#ifndef AQUILA_CORE_TRADING_ORDER_LATENCY_H_
#define AQUILA_CORE_TRADING_ORDER_LATENCY_H_

#include <cstdint>

#include "core/trading/order_types.h"

namespace aquila::core {

struct StrategyOrderTimingSnapshot {
  std::int64_t request_send_local_ns{0};
  std::int64_t ack_local_receive_ns{0};
  std::int64_t response_local_receive_ns{0};
  std::int64_t ack_exchange_ns{0};
  std::int64_t response_exchange_ns{0};
  std::int64_t accepted_exchange_ns{0};
  std::int64_t finish_exchange_ns{0};
  std::int64_t ack_rtt_ns{0};
  std::int64_t response_rtt_ns{0};
  std::int64_t ack_exchange_to_local_ns{0};
  std::int64_t response_exchange_to_local_ns{0};
  std::int64_t exchange_lifecycle_ns{0};
};

[[nodiscard]] constexpr std::int64_t LatencyDeltaNs(
    std::int64_t end_ns, std::int64_t start_ns) noexcept {
  if (end_ns == 0 || start_ns == 0) {
    return 0;
  }
  return end_ns - start_ns;
}

[[nodiscard]] constexpr StrategyOrderTimingSnapshot
MakeStrategyOrderTimingSnapshot(const StrategyOrder& order) noexcept {
  return StrategyOrderTimingSnapshot{
      .request_send_local_ns = order.request_send_local_ns,
      .ack_local_receive_ns = order.ack_local_receive_ns,
      .response_local_receive_ns = order.response_local_receive_ns,
      .ack_exchange_ns = order.ack_exchange_ns,
      .response_exchange_ns = order.response_exchange_ns,
      .accepted_exchange_ns = order.accepted_exchange_ns,
      .finish_exchange_ns = order.finish_exchange_ns,
      .ack_rtt_ns = LatencyDeltaNs(order.ack_local_receive_ns,
                                   order.request_send_local_ns),
      .response_rtt_ns = LatencyDeltaNs(order.response_local_receive_ns,
                                        order.request_send_local_ns),
      .ack_exchange_to_local_ns =
          LatencyDeltaNs(order.ack_local_receive_ns, order.ack_exchange_ns),
      .response_exchange_to_local_ns = LatencyDeltaNs(
          order.response_local_receive_ns, order.response_exchange_ns),
      .exchange_lifecycle_ns =
          LatencyDeltaNs(order.finish_exchange_ns, order.accepted_exchange_ns),
  };
}

}  // namespace aquila::core

#endif  // AQUILA_CORE_TRADING_ORDER_LATENCY_H_
