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
  // Inbound frames dropped because the consumer returned kBackpressured.
  std::uint64_t consumer_backpressure_drops{0};
  // Control frames (auto-pong / heartbeat ping) skipped due to slot exhaustion.
  std::uint64_t control_frame_enqueue_failures{0};
  // Bounded codec storage could not make progress without overwriting data.
  std::uint64_t frame_codec_capacity_exhaustions{0};
  std::uint64_t frame_codec_ready_high_watermark{0};
  std::uint64_t degraded_enter_count{0};
  std::uint64_t degraded_exit_count{0};
  std::uint64_t degraded_active{0};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_METRICS_H_
