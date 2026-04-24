#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace aquila::websocket::benchmarking {

inline std::uint64_t NowNs() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

inline bool SetNonBlocking(int fd) noexcept {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

class LocalFdSocket {
 public:
  explicit LocalFdSocket(int fd = -1) noexcept : fd_(fd) {}

  ~LocalFdSocket() noexcept { Close(); }

  LocalFdSocket(const LocalFdSocket&) = delete;
  LocalFdSocket& operator=(const LocalFdSocket&) = delete;

  LocalFdSocket(LocalFdSocket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  LocalFdSocket& operator=(LocalFdSocket&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  ssize_t ReadSome(std::span<std::byte> buffer) noexcept {
    const ssize_t result = ::read(fd_, buffer.data(), buffer.size());
    if (result >= 0) {
      return result;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      errno = EAGAIN;
      return -1;
    }
    return -1;
  }

  ssize_t WriteSome(std::span<const std::byte> buffer) noexcept {
    const ssize_t result = ::write(fd_, buffer.data(), buffer.size());
    if (result >= 0) {
      return result;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      errno = EAGAIN;
      return -1;
    }
    return -1;
  }

  void Close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_{-1};
};

struct SocketPair {
  LocalFdSocket client{};
  int peer_fd{-1};

  ~SocketPair() noexcept {
    if (peer_fd >= 0) {
      ::close(peer_fd);
      peer_fd = -1;
    }
  }
};

inline bool CreateSocketPair(SocketPair& pair) noexcept {
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return false;
  }
  if (!SetNonBlocking(fds[0]) || !SetNonBlocking(fds[1])) {
    ::close(fds[0]);
    ::close(fds[1]);
    return false;
  }

  pair.client = LocalFdSocket(fds[0]);
  pair.peer_fd = fds[1];
  return true;
}

inline bool WriteAllFd(int fd, std::span<const std::byte> bytes) noexcept {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written > 0) {
      offset += static_cast<size_t>(written);
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::yield();
      continue;
    }
    return false;
  }
  return true;
}

inline bool ReadExactFd(int fd, std::span<std::byte> bytes) noexcept {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t read_bytes =
        ::read(fd, bytes.data() + offset, bytes.size() - offset);
    if (read_bytes > 0) {
      offset += static_cast<size_t>(read_bytes);
      continue;
    }
    if (read_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::yield();
      continue;
    }
    return false;
  }
  return true;
}

inline std::vector<std::byte> BuildServerTextFrame(
    std::string_view payload) noexcept {
  std::vector<std::byte> frame(2 + payload.size());
  frame[0] = std::byte{0x81};
  frame[1] = std::byte{static_cast<unsigned char>(payload.size())};
  for (size_t i = 0; i < payload.size(); ++i) {
    frame[2 + i] = std::byte{static_cast<unsigned char>(payload[i])};
  }
  return frame;
}

inline std::array<std::byte, 64> BuildWritePayload() noexcept {
  std::array<std::byte, 64> payload{};
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = std::byte{static_cast<unsigned char>(i)};
  }
  return payload;
}

}  // namespace aquila::websocket::benchmarking
