#ifndef AQUILA_CORE_WEBSOCKET_SOCKET_DIAGNOSTICS_H_
#define AQUILA_CORE_WEBSOCKET_SOCKET_DIAGNOSTICS_H_

#include <sys/socket.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#if defined(__linux__)
#include <sys/ioctl.h>

#include <linux/sockios.h>
#endif

#include "core/common/order_ack_diagnostic_level.h"

namespace aquila::websocket {

inline constexpr std::size_t kSocketIpTextCapacity = INET6_ADDRSTRLEN;

struct SocketEndpointDiagnostics {
  bool available{false};
  std::array<char, kSocketIpTextCapacity> local_ip{};
  std::uint16_t local_port{0};
  std::array<char, kSocketIpTextCapacity> remote_ip{};
  std::uint16_t remote_port{0};
};

struct TcpInfoDiagnostics {
  bool available{false};
  std::uint32_t rtt_us{0};
  std::uint32_t rttvar_us{0};
  std::uint32_t retrans{0};
  std::uint32_t total_retrans{0};
  std::uint32_t unacked{0};
  std::uint32_t snd_cwnd{0};
};

struct SocketSendQueueDiagnostics {
  bool available{false};
  std::uint32_t sendq_bytes{0};
  std::uint32_t notsent_bytes{0};
};

namespace detail {

[[nodiscard]] inline bool FormatSockaddrIpAndPort(
    const sockaddr_storage& address,
    std::array<char, kSocketIpTextCapacity>& ip, std::uint16_t& port) noexcept {
  if (address.ss_family == AF_INET) {
    const auto* in = reinterpret_cast<const sockaddr_in*>(&address);
    if (::inet_ntop(AF_INET, &in->sin_addr, ip.data(), ip.size()) == nullptr) {
      return false;
    }
    port = ntohs(in->sin_port);
    return true;
  }
  if (address.ss_family == AF_INET6) {
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&address);
    if (::inet_ntop(AF_INET6, &in6->sin6_addr, ip.data(), ip.size()) ==
        nullptr) {
      return false;
    }
    port = ntohs(in6->sin6_port);
    return true;
  }
  return false;
}

}  // namespace detail

[[nodiscard]] inline SocketEndpointDiagnostics
SnapshotSocketEndpointDiagnostics(int fd) noexcept {
  SocketEndpointDiagnostics snapshot{};
  if (fd < 0) {
    return snapshot;
  }

  sockaddr_storage local_address{};
  socklen_t local_address_len = sizeof(local_address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local_address),
                    &local_address_len) != 0) {
    return snapshot;
  }

  sockaddr_storage remote_address{};
  socklen_t remote_address_len = sizeof(remote_address);
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&remote_address),
                    &remote_address_len) != 0) {
    return snapshot;
  }

  if (!detail::FormatSockaddrIpAndPort(local_address, snapshot.local_ip,
                                       snapshot.local_port) ||
      !detail::FormatSockaddrIpAndPort(remote_address, snapshot.remote_ip,
                                       snapshot.remote_port)) {
    return SocketEndpointDiagnostics{};
  }

  snapshot.available = true;
  return snapshot;
}

[[nodiscard]] inline TcpInfoDiagnostics SnapshotTcpInfoDiagnostics(
    int fd) noexcept {
  TcpInfoDiagnostics snapshot{};
  if constexpr (!::aquila::core::kOrderAckDiagnosticTcpInfoEnabled) {
    (void)fd;
    return snapshot;
  } else {
#if defined(__linux__)
    if (fd < 0) {
      return snapshot;
    }
    tcp_info info{};
    socklen_t info_len = sizeof(info);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &info_len) != 0) {
      return snapshot;
    }
    snapshot.available = true;
    snapshot.rtt_us = info.tcpi_rtt;
    snapshot.rttvar_us = info.tcpi_rttvar;
    snapshot.retrans = info.tcpi_retrans;
    snapshot.total_retrans = info.tcpi_total_retrans;
    snapshot.unacked = info.tcpi_unacked;
    snapshot.snd_cwnd = info.tcpi_snd_cwnd;
#else
    (void)fd;
#endif
    return snapshot;
  }
}

[[nodiscard]] inline SocketSendQueueDiagnostics
SnapshotSocketSendQueueDiagnostics(int fd) noexcept {
  SocketSendQueueDiagnostics snapshot{};
  if constexpr (!::aquila::core::kOrderAckDiagnosticTcpInfoEnabled) {
    (void)fd;
    return snapshot;
  } else {
#if defined(__linux__)
    if (fd < 0) {
      return snapshot;
    }
    int sendq_bytes = 0;
    if (::ioctl(fd, SIOCOUTQ, &sendq_bytes) == 0 && sendq_bytes >= 0) {
      snapshot.available = true;
      snapshot.sendq_bytes = static_cast<std::uint32_t>(sendq_bytes);
    }
#if defined(SIOCOUTQNSD)
    int notsent_bytes = 0;
    if (::ioctl(fd, SIOCOUTQNSD, &notsent_bytes) == 0 &&
        notsent_bytes >= 0) {
      snapshot.available = true;
      snapshot.notsent_bytes = static_cast<std::uint32_t>(notsent_bytes);
    }
#endif
#else
    (void)fd;
#endif
    return snapshot;
  }
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_SOCKET_DIAGNOSTICS_H_
