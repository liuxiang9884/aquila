#ifndef AQUILA_CORE_WEBSOCKET_SOCKET_TIMESTAMPING_H_
#define AQUILA_CORE_WEBSOCKET_SOCKET_TIMESTAMPING_H_

#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <type_traits>

#if defined(__linux__)
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#endif

#ifndef AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
#define AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION 1
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
  std::uint32_t max_active_probes{16'384};
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

enum class SocketTimestampingEventKind : std::uint8_t {
  kUnknown,
  kTxSoftware,
  kTxSched,
  kTxAck,
  kRxSoftware,
};

struct SocketTimestampingEvent {
  SocketTimestampingEventKind kind{SocketTimestampingEventKind::kUnknown};
  std::int64_t timestamp_ns{0};
  std::uint32_t id{0};
};

inline constexpr std::size_t kSocketTimestampingMaxDrainEvents = 32;

struct SocketTimestampingEventDrain {
  bool ok{true};
  int error_errno{0};
  std::uint32_t events_seen{0};
  std::array<SocketTimestampingEvent, kSocketTimestampingMaxDrainEvents>
      events{};
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
      .rx_software_to_ack_receive_ns = DiffOrMinusOne(
          snapshot.ack_receive_local_ns, snapshot.rx_software_ns),
  };
}

[[nodiscard]] inline SocketTimestampingApplyResult
ApplySocketTimestampingConfig(int fd,
                              const SocketTimestampingConfig& config) noexcept {
#if !AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
  (void)fd;
  (void)config;
  return SocketTimestampingApplyResult{.ok = true, .enabled = false};
#else
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
#endif
}

namespace detail {

template <typename TimespecT>
[[nodiscard]] inline std::int64_t TimespecToNs(
    const TimespecT& value) noexcept {
  return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000LL +
         static_cast<std::int64_t>(value.tv_nsec);
}

template <typename TimestampingT>
[[nodiscard]] inline std::int64_t FirstTimestampNs(
    const TimestampingT& timestamps) noexcept {
  const std::int64_t software_ns = TimespecToNs(timestamps.ts[0]);
  if (software_ns > 0) {
    return software_ns;
  }
  return TimespecToNs(timestamps.ts[2]);
}

[[nodiscard]] inline SocketTimestampingEventKind KindForTimestampInfo(
    std::uint32_t info) noexcept {
#if defined(__linux__)
  switch (info) {
    case SCM_TSTAMP_SND:
      return SocketTimestampingEventKind::kTxSoftware;
    case SCM_TSTAMP_SCHED:
      return SocketTimestampingEventKind::kTxSched;
    case SCM_TSTAMP_ACK:
      return SocketTimestampingEventKind::kTxAck;
    default:
      return SocketTimestampingEventKind::kUnknown;
  }
#else
  (void)info;
  return SocketTimestampingEventKind::kUnknown;
#endif
}

}  // namespace detail

[[nodiscard]] inline SocketTimestampingEventDrain
DrainSocketTimestampingErrorQueue(int fd, std::uint32_t max_events) noexcept {
  SocketTimestampingEventDrain drain{};
#if !AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
  (void)fd;
  (void)max_events;
  return drain;
#elif defined(__linux__)
  if (fd < 0) {
    return SocketTimestampingEventDrain{
        .ok = false, .error_errno = EBADF, .events_seen = 0};
  }

  const std::uint32_t event_limit =
      max_events < kSocketTimestampingMaxDrainEvents
          ? max_events
          : static_cast<std::uint32_t>(kSocketTimestampingMaxDrainEvents);
  while (drain.events_seen < event_limit) {
    std::array<char, 256> control{};
    std::array<char, 1> payload{};
    iovec iov{.iov_base = payload.data(), .iov_len = payload.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();

    const ssize_t received = ::recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
    if (received < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return drain;
      }
      drain.ok = false;
      drain.error_errno = errno;
      return drain;
    }

    std::int64_t timestamp_ns = 0;
    SocketTimestampingEventKind kind = SocketTimestampingEventKind::kUnknown;
    std::uint32_t id = 0;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET &&
          cmsg->cmsg_type == SCM_TIMESTAMPING) {
        const auto* timestamps =
            reinterpret_cast<const scm_timestamping*>(CMSG_DATA(cmsg));
        timestamp_ns = detail::FirstTimestampNs(*timestamps);
      }
#if defined(SCM_TIMESTAMPING_NEW)
      if (cmsg->cmsg_level == SOL_SOCKET &&
          cmsg->cmsg_type == SCM_TIMESTAMPING_NEW) {
        const auto* timestamps =
            reinterpret_cast<const scm_timestamping64*>(CMSG_DATA(cmsg));
        timestamp_ns = detail::FirstTimestampNs(*timestamps);
      }
#endif
      if ((cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) ||
          (cmsg->cmsg_level == SOL_IPV6 && cmsg->cmsg_type == IPV6_RECVERR)) {
        const auto* extended_error =
            reinterpret_cast<const sock_extended_err*>(CMSG_DATA(cmsg));
        if (extended_error->ee_origin == SO_EE_ORIGIN_TIMESTAMPING) {
          kind = detail::KindForTimestampInfo(extended_error->ee_info);
          id = extended_error->ee_data;
        }
      }
    }
    if (timestamp_ns <= 0) {
      continue;
    }
    drain.events[drain.events_seen] = SocketTimestampingEvent{
        .kind = kind, .timestamp_ns = timestamp_ns, .id = id};
    ++drain.events_seen;
  }
  return drain;
#else
  (void)fd;
  (void)max_events;
  return SocketTimestampingEventDrain{
      .ok = false, .error_errno = ENOTSUP, .events_seen = 0};
#endif
}

[[nodiscard]] inline std::int64_t ExtractRxSoftwareTimestampNs(
    const msghdr& msg) noexcept {
#if !AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
  (void)msg;
#elif defined(__linux__)
  for (cmsghdr* cmsg = CMSG_FIRSTHDR(const_cast<msghdr*>(&msg));
       cmsg != nullptr; cmsg = CMSG_NXTHDR(const_cast<msghdr*>(&msg), cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING) {
      const auto* timestamps =
          reinterpret_cast<const scm_timestamping*>(CMSG_DATA(cmsg));
      return detail::TimespecToNs(timestamps->ts[0]);
    }
#if defined(SCM_TIMESTAMPING_NEW)
    if (cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_TIMESTAMPING_NEW) {
      const auto* timestamps =
          reinterpret_cast<const scm_timestamping64*>(CMSG_DATA(cmsg));
      return detail::TimespecToNs(timestamps->ts[0]);
    }
#endif
  }
#else
  (void)msg;
#endif
  return 0;
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_SOCKET_TIMESTAMPING_H_
