#ifndef AQUILA_CORE_WEBSOCKET_PLAIN_SOCKET_H_
#define AQUILA_CORE_WEBSOCKET_PLAIN_SOCKET_H_

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <span>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "core/websocket/types.h"

namespace aquila::websocket {

class PlainSocket {
 public:
  static constexpr bool kUsesTls = false;

  PlainSocket() = default;

  ~PlainSocket() noexcept { Close(); }

  PlainSocket(const PlainSocket&) = delete;
  PlainSocket& operator=(const PlainSocket&) = delete;

  PlainSocket(PlainSocket&& other) noexcept { MoveFrom(other); }

  PlainSocket& operator=(PlainSocket&& other) noexcept {
    if (this != &other) {
      Close();
      MoveFrom(other);
    }
    return *this;
  }

  bool Init() noexcept {
    IgnoreSigpipeOnce();
    wants_read_ = false;
    wants_write_ = false;
    connect_pending_ = false;
    return true;
  }

  bool OpenAndConnect(const ConnectionConfig& config) noexcept {
    wants_read_ = false;
    wants_write_ = false;
    connect_pending_ = false;

    if (!Init()) {
      return false;
    }

    Close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    if (getaddrinfo(config.host.c_str(), config.service.c_str(), &hints,
                    &addresses) != 0) {
      return false;
    }

    bool connected = false;
    for (addrinfo* current = addresses; current != nullptr;
         current = current->ai_next) {
      const int fd =
          ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
      if (fd < 0) {
        continue;
      }

      if (!SetNonBlocking(fd)) {
        ::close(fd);
        continue;
      }

      const int result = ::connect(fd, current->ai_addr, current->ai_addrlen);
      if (result == 0) {
        fd_ = fd;
        connected = true;
        break;
      }

      if (result < 0 && errno == EINPROGRESS) {
        fd_ = fd;
        connected = true;
        connect_pending_ = true;
        wants_write_ = true;
        break;
      }

      ::close(fd);
    }
    freeaddrinfo(addresses);
    return connected;
  }

  bool FinishHandshake() noexcept {
    wants_read_ = false;
    wants_write_ = false;
    if (fd_ < 0) {
      return false;
    }
    if (!connect_pending_) {
      return true;
    }

    int error = 0;
    socklen_t error_len = sizeof(error);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &error_len) != 0) {
      return false;
    }
    if (error == 0) {
      connect_pending_ = false;
      return true;
    }
    if (error == EINPROGRESS || error == EALREADY) {
      wants_write_ = true;
      return false;
    }
    errno = error;
    return false;
  }

  ssize_t ReadSome(std::span<std::byte> buffer) noexcept {
    wants_read_ = false;
    wants_write_ = false;
    if (fd_ < 0 || buffer.empty()) {
      return -1;
    }
    const ssize_t result = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      wants_read_ = true;
    }
    return result;
  }

  size_t PendingReadableBytes() const noexcept { return 0; }

  ssize_t WriteSome(std::span<const std::byte> buffer) noexcept {
    wants_read_ = false;
    wants_write_ = false;
    if (fd_ < 0 || buffer.empty()) {
      return -1;
    }
    const ssize_t result =
        ::send(fd_, buffer.data(), buffer.size(), MSG_NOSIGNAL);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      wants_write_ = true;
    }
    return result;
  }

  bool WantsRead() const noexcept { return wants_read_; }

  bool WantsWrite() const noexcept { return wants_write_; }

  int NativeFd() const noexcept { return fd_; }

  void Close() noexcept {
    wants_read_ = false;
    wants_write_ = false;
    connect_pending_ = false;
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  static bool SetNonBlocking(int fd) noexcept {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
      return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  static void IgnoreSigpipeOnce() noexcept {
    static const bool ignored = [] {
      std::signal(SIGPIPE, SIG_IGN);
      return true;
    }();
    (void)ignored;
  }

  void MoveFrom(PlainSocket& other) noexcept {
    fd_ = other.fd_;
    wants_read_ = other.wants_read_;
    wants_write_ = other.wants_write_;
    connect_pending_ = other.connect_pending_;

    other.fd_ = -1;
    other.wants_read_ = false;
    other.wants_write_ = false;
    other.connect_pending_ = false;
  }

  int fd_{-1};
  bool wants_read_{false};
  bool wants_write_{false};
  bool connect_pending_{false};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_PLAIN_SOCKET_H_
