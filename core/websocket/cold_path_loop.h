#ifndef AQUILA_CORE_WEBSOCKET_COLD_PATH_LOOP_H_
#define AQUILA_CORE_WEBSOCKET_COLD_PATH_LOOP_H_

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include <sys/epoll.h>
#include <unistd.h>

#include "core/websocket/handshake.h"
#include "core/websocket/state_machine.h"
#include "core/websocket/tls_socket.h"

namespace aquila::websocket {

class ColdPathLoop {
 public:
  ColdPathLoop() = default;

  ~ColdPathLoop() noexcept {
    if (epoll_fd_ >= 0) {
      ::close(epoll_fd_);
    }
  }

  ColdPathLoop(const ColdPathLoop&) = delete;
  ColdPathLoop& operator=(const ColdPathLoop&) = delete;

  void SetInterruptFd(int fd) noexcept { interrupt_fd_ = fd; }

  bool Init() noexcept {
    if (epoll_fd_ >= 0) {
      ::close(epoll_fd_);
      epoll_fd_ = -1;
    }
    epoll_fd_ = epoll_create1(0);
    return epoll_fd_ >= 0;
  }

  bool RunUntilActive(TlsSocket& socket, StateMachine& state_machine,
                      const ConnectionConfig& config,
                      std::span<char> handshake_storage) noexcept {
    if (!Init()) {
      state_machine.Fail(ConnectionError::kSocketError,
                         ConnectionPhase::kTcpConnecting);
      return false;
    }
    if (handshake_storage.empty()) {
      state_machine.Fail(ConnectionError::kHandshakeFailure,
                         ConnectionPhase::kWsHandshaking);
      return false;
    }

    const Deadline deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(config.cold_path_total_timeout_ms);

    state_machine.Enter(ConnectionPhase::kResolving);
    state_machine.Enter(ConnectionPhase::kTcpConnecting);
    // TODO: move synchronous DNS off the cold path if it becomes a jitter
    // source; currently it runs inside OpenAndConnect without interruption.
    if (!socket.OpenAndConnect(config)) {
      state_machine.Fail(ConnectionError::kSocketError,
                         ConnectionPhase::kTcpConnecting);
      return false;
    }

    state_machine.Enter(ConnectionPhase::kTlsHandshaking);
    while (!socket.FinishHandshake()) {
      const WaitOutcome outcome = WaitForSocket(socket, deadline);
      if (outcome == WaitOutcome::kInterrupted) {
        state_machine.Enter(ConnectionPhase::kClosing);
        return false;
      }
      if (outcome == WaitOutcome::kTimeout) {
        state_machine.Fail(ConnectionError::kConnectTimeout,
                           ConnectionPhase::kTlsHandshaking);
        return false;
      }
      if (outcome == WaitOutcome::kFailure) {
        state_machine.Fail(ConnectionError::kTlsFailure,
                           ConnectionPhase::kTlsHandshaking);
        return false;
      }
    }

    state_machine.Enter(ConnectionPhase::kWsHandshaking);
    const std::string_view client_key = GenerateClientKey(client_key_storage_);
    if (client_key.empty()) {
      state_machine.Fail(ConnectionError::kHandshakeFailure,
                         ConnectionPhase::kWsHandshaking);
      return false;
    }
    auto built = BuildClientHandshake(config.host, config.target, client_key,
                                      handshake_storage);
    if (!built.ok) {
      state_machine.Fail(ConnectionError::kHandshakeFailure,
                         ConnectionPhase::kWsHandshaking);
      return false;
    }
    const WaitOutcome write_outcome = WriteAll(socket, built.bytes, deadline);
    if (write_outcome == WaitOutcome::kInterrupted) {
      state_machine.Enter(ConnectionPhase::kClosing);
      return false;
    }
    if (write_outcome == WaitOutcome::kTimeout) {
      state_machine.Fail(ConnectionError::kConnectTimeout,
                         ConnectionPhase::kWsHandshaking);
      return false;
    }
    if (write_outcome != WaitOutcome::kReady) {
      state_machine.Fail(ConnectionError::kHandshakeFailure,
                         ConnectionPhase::kWsHandshaking);
      return false;
    }

    size_t response_bytes = 0;
    while (response_bytes < handshake_storage.size()) {
      std::string_view response(handshake_storage.data(), response_bytes);
      if (response.find("\r\n\r\n") != std::string_view::npos) {
        if (!ValidateServerHandshake(response, client_key)) {
          state_machine.Fail(ConnectionError::kHandshakeFailure,
                             ConnectionPhase::kWsHandshaking);
          return false;
        }
        state_machine.Enter(ConnectionPhase::kActive);
        return true;
      }

      std::span<char> remaining(handshake_storage.data() + response_bytes,
                                handshake_storage.size() - response_bytes);
      const ssize_t received =
          socket.ReadSome(std::as_writable_bytes(remaining));
      if (received > 0) {
        response_bytes += static_cast<size_t>(received);
        continue;
      }
      if (received == 0) {
        break;
      }
      if (errno == EAGAIN) {
        const WaitOutcome outcome = WaitForSocket(socket, deadline);
        if (outcome == WaitOutcome::kInterrupted) {
          state_machine.Enter(ConnectionPhase::kClosing);
          return false;
        }
        if (outcome == WaitOutcome::kTimeout) {
          state_machine.Fail(ConnectionError::kConnectTimeout,
                             ConnectionPhase::kWsHandshaking);
          return false;
        }
        if (outcome == WaitOutcome::kReady) {
          continue;
        }
      }
      break;
    }

    state_machine.Fail(ConnectionError::kHandshakeFailure,
                       ConnectionPhase::kWsHandshaking);
    return false;
  }

 private:
  enum class WaitOutcome : std::uint8_t {
    kReady,
    kTimeout,
    kFailure,
    kInterrupted
  };
  using Deadline = std::chrono::steady_clock::time_point;

  static int RemainingMs(Deadline deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();
    return static_cast<int>(std::min<std::int64_t>(
        remaining, std::numeric_limits<int>::max()));
  }

  WaitOutcome WaitForSocket(TlsSocket& socket, Deadline deadline) noexcept {
    const uint32_t events = (socket.WantsRead() ? EPOLLIN : 0U) |
                            (socket.WantsWrite() ? EPOLLOUT : 0U);
    if (socket.NativeFd() < 0 || events == 0U) {
      return WaitOutcome::kFailure;
    }

    epoll_event event{};
    event.events = events;
    event.data.fd = socket.NativeFd();
    if (!ArmFd(socket.NativeFd(), event)) {
      return WaitOutcome::kFailure;
    }
    if (interrupt_fd_ >= 0) {
      epoll_event interrupt_event{};
      interrupt_event.events = EPOLLIN;
      interrupt_event.data.fd = interrupt_fd_;
      if (!ArmFd(interrupt_fd_, interrupt_event)) {
        return WaitOutcome::kFailure;
      }
    }

    epoll_event ready{};
    while (true) {
      const int timeout_ms = RemainingMs(deadline);
      const int ready_count = epoll_wait(epoll_fd_, &ready, 1, timeout_ms);
      if (ready_count > 0) {
        if (ready.data.fd == interrupt_fd_) {
          DrainInterruptFd();
          return WaitOutcome::kInterrupted;
        }
        if ((ready.events & (EPOLLERR | EPOLLHUP)) != 0) {
          return WaitOutcome::kFailure;
        }
        return WaitOutcome::kReady;
      }
      if (ready_count == 0) {
        return WaitOutcome::kTimeout;
      }
      if (errno == EINTR) {
        continue;
      }
      return WaitOutcome::kFailure;
    }
  }

  WaitOutcome WriteAll(TlsSocket& socket, std::string_view bytes,
                       Deadline deadline) noexcept {
    std::span<const char> chars(bytes.data(), bytes.size());
    const auto payload = std::as_bytes(chars);
    size_t offset = 0;
    while (offset < payload.size()) {
      const std::span<const std::byte> remaining(payload.data() + offset,
                                                 payload.size() - offset);
      const ssize_t written = socket.WriteSome(remaining);
      if (written > 0) {
        offset += static_cast<size_t>(written);
        continue;
      }
      if (written == 0) {
        return WaitOutcome::kFailure;
      }
      if (errno != EAGAIN) {
        return WaitOutcome::kFailure;
      }
      const WaitOutcome outcome = WaitForSocket(socket, deadline);
      if (outcome == WaitOutcome::kInterrupted) {
        return outcome;
      }
      if (outcome != WaitOutcome::kReady) {
        return outcome;
      }
    }
    return WaitOutcome::kReady;
  }

  bool ArmFd(int fd, epoll_event event) noexcept {
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == 0) {
      return true;
    }
    if (errno != EEXIST) {
      return false;
    }
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) == 0) {
      return true;
    }
    return errno == ENOENT &&
           epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == 0;
  }

  void DrainInterruptFd() noexcept {
    if (interrupt_fd_ < 0) {
      return;
    }
    std::uint64_t value = 0;
    while (::read(interrupt_fd_, &value, sizeof(value)) ==
           static_cast<ssize_t>(sizeof(value))) {
    }
  }

  int epoll_fd_{-1};
  int interrupt_fd_{-1};
  std::array<char, 32> client_key_storage_{};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_COLD_PATH_LOOP_H_
