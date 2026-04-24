#ifndef AQUILA_CORE_WEBSOCKET_COLD_PATH_LOOP_H_
#define AQUILA_CORE_WEBSOCKET_COLD_PATH_LOOP_H_

#include <array>
#include <cerrno>
#include <cstddef>
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

  bool Init() noexcept {
    if (epoll_fd_ >= 0) {
      return true;
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

    state_machine.Enter(ConnectionPhase::kResolving);
    state_machine.Enter(ConnectionPhase::kTcpConnecting);
    if (!socket.OpenAndConnect(config)) {
      state_machine.Fail(ConnectionError::kSocketError,
                         ConnectionPhase::kTcpConnecting);
      return false;
    }

    state_machine.Enter(ConnectionPhase::kTlsHandshaking);
    while (!socket.FinishHandshake()) {
      if (!WaitForSocket(socket)) {
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
    if (!built.ok || !WriteAll(socket, built.bytes)) {
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
      if (errno == EAGAIN && WaitForSocket(socket)) {
        continue;
      }
      break;
    }

    state_machine.Fail(ConnectionError::kHandshakeFailure,
                       ConnectionPhase::kWsHandshaking);
    return false;
  }

 private:
  bool WaitForSocket(TlsSocket& socket) noexcept {
    const uint32_t events = (socket.WantsRead() ? EPOLLIN : 0U) |
                            (socket.WantsWrite() ? EPOLLOUT : 0U);
    if (socket.NativeFd() < 0 || events == 0U) {
      return false;
    }

    epoll_event event{};
    event.events = events;
    event.data.fd = socket.NativeFd();
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socket.NativeFd(), &event) != 0) {
      if (errno != EEXIST) {
        return false;
      }
      if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, socket.NativeFd(), &event) != 0) {
        if (errno != ENOENT ||
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socket.NativeFd(), &event) !=
                0) {
          return false;
        }
      }
    }

    epoll_event ready{};
    while (true) {
      const int ready_count = epoll_wait(epoll_fd_, &ready, 1, -1);
      if (ready_count > 0) {
        return (ready.events & (EPOLLERR | EPOLLHUP)) == 0;
      }
      if (ready_count < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
  }

  bool WriteAll(TlsSocket& socket, std::string_view bytes) noexcept {
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
        return false;
      }
      if (errno != EAGAIN) {
        return false;
      }
      if (!WaitForSocket(socket)) {
        return false;
      }
    }
    return true;
  }

  int epoll_fd_{-1};
  std::array<char, 32> client_key_storage_{};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_COLD_PATH_LOOP_H_
