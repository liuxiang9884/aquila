#ifndef AQUILA_CORE_WEBSOCKET_METRICS_H_
#define AQUILA_CORE_WEBSOCKET_METRICS_H_

#include <cstdint>

namespace aquila::websocket {

struct Metrics {
  std::uint64_t rx_bytes{0};
  std::uint64_t tx_bytes{0};
  std::uint64_t rx_messages{0};
  std::uint64_t tx_messages{0};
  std::uint64_t reconnects{0};
  std::uint64_t spin_iterations{0};
  // Peak number of prepared-write slots simultaneously in use by the owner.
  std::uint64_t prepared_write_high_watermark{0};
  std::uint64_t heartbeat_timeouts{0};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_METRICS_H_
