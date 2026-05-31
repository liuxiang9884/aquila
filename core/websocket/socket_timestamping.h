#ifndef AQUILA_CORE_WEBSOCKET_SOCKET_TIMESTAMPING_H_
#define AQUILA_CORE_WEBSOCKET_SOCKET_TIMESTAMPING_H_

#include <cerrno>
#include <cstdint>
#include <sys/socket.h>

#if defined(__linux__)
#include <linux/net_tstamp.h>
#endif

namespace aquila::websocket {

struct SocketTimestampingConfig {
  bool enabled{false};
  bool tx_sched{false};
  bool tx_software{false};
  bool tx_ack{false};
  bool rx_software{false};
  bool hardware{false};
  std::uint32_t max_errqueue_events_per_drain{16};
};

struct SocketTimestampingSnapshot {
  bool available{false};
  std::int64_t write_complete_ns{0};
  std::int64_t tx_sched_ns{0};
  std::int64_t tx_software_ns{0};
  std::int64_t tx_ack_ns{0};
  std::int64_t rx_software_ns{0};
  std::int64_t ack_receive_local_ns{0};
};

struct SocketTimestampingStages {
  std::int64_t write_complete_to_tx_software_ns{-1};
  std::int64_t tx_software_to_tx_ack_ns{-1};
  std::int64_t tx_ack_to_rx_software_ns{-1};
  std::int64_t rx_software_to_ack_receive_ns{-1};
};

struct SocketTimestampingApplyResult {
  bool ok{false};
  bool enabled{false};
  int error_errno{0};
};

[[nodiscard]] inline std::int64_t DiffOrMinusOne(
    std::int64_t end_ns, std::int64_t begin_ns) noexcept {
  return end_ns > 0 && begin_ns > 0 && end_ns >= begin_ns ? end_ns - begin_ns
                                                          : -1;
}

[[nodiscard]] inline SocketTimestampingStages ComputeSocketTimestampingStages(
    const SocketTimestampingSnapshot& snapshot) noexcept {
  return SocketTimestampingStages{
      .write_complete_to_tx_software_ns =
          DiffOrMinusOne(snapshot.tx_software_ns, snapshot.write_complete_ns),
      .tx_software_to_tx_ack_ns =
          DiffOrMinusOne(snapshot.tx_ack_ns, snapshot.tx_software_ns),
      .tx_ack_to_rx_software_ns =
          DiffOrMinusOne(snapshot.rx_software_ns, snapshot.tx_ack_ns),
      .rx_software_to_ack_receive_ns =
          DiffOrMinusOne(snapshot.ack_receive_local_ns,
                         snapshot.rx_software_ns),
  };
}

[[nodiscard]] inline SocketTimestampingApplyResult
ApplySocketTimestampingConfig(int fd,
                              const SocketTimestampingConfig& config) noexcept {
  if (!config.enabled) {
    return SocketTimestampingApplyResult{.ok = true, .enabled = false};
  }
#if defined(__linux__)
  if (fd < 0) {
    return SocketTimestampingApplyResult{
        .ok = false, .enabled = false, .error_errno = EBADF};
  }

  int flags = SOF_TIMESTAMPING_OPT_TSONLY | SOF_TIMESTAMPING_OPT_ID;
#if defined(SOF_TIMESTAMPING_OPT_ID_TCP)
  flags |= SOF_TIMESTAMPING_OPT_ID_TCP;
#endif
  if (config.tx_sched) {
    flags |= SOF_TIMESTAMPING_TX_SCHED | SOF_TIMESTAMPING_SOFTWARE;
  }
  if (config.tx_software) {
    flags |= SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
  }
  if (config.tx_ack) {
    flags |= SOF_TIMESTAMPING_TX_ACK | SOF_TIMESTAMPING_SOFTWARE;
  }
  if (config.rx_software) {
    flags |= SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
  }
  if (config.hardware) {
    flags |= SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE |
             SOF_TIMESTAMPING_RAW_HARDWARE;
  }

  if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) !=
      0) {
    return SocketTimestampingApplyResult{
        .ok = false, .enabled = false, .error_errno = errno};
  }
  return SocketTimestampingApplyResult{.ok = true, .enabled = true};
#else
  (void)fd;
  return SocketTimestampingApplyResult{
      .ok = false, .enabled = false, .error_errno = ENOTSUP};
#endif
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_SOCKET_TIMESTAMPING_H_
