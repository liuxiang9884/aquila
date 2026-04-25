#include "core/websocket/cold_path_loop.h"
#include "core/websocket/state_machine.h"
#include "core/websocket/tls_socket.h"
#include "core/websocket/types.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace aquila::websocket;

namespace {

// TCP server that accepts connections but never sends bytes. Drives the TLS
// handshake into SSL_ERROR_WANT_READ so ColdPathLoop blocks on epoll.
class BlackholeTcpServer {
 public:
  ~BlackholeTcpServer() noexcept {
    stop_.store(true, std::memory_order_relaxed);
    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }
    for (int fd : accepted_) {
      ::close(fd);
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
    }
  }

  bool Start() noexcept {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return false;
    }
    int enable = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
        0) {
      return false;
    }
    if (::listen(listen_fd_, 4) != 0) {
      return false;
    }

    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&actual),
                      &len) != 0) {
      return false;
    }
    port_ = ntohs(actual.sin_port);

    accept_thread_ = std::thread([this] { AcceptLoop(); });
    return true;
  }

  int port() const noexcept { return port_; }

 private:
  void AcceptLoop() noexcept {
    while (!stop_.load(std::memory_order_relaxed)) {
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(listen_fd_, &read_set);
      timeval tv{0, 50'000};
      const int ready =
          ::select(listen_fd_ + 1, &read_set, nullptr, nullptr, &tv);
      if (ready > 0) {
        const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd >= 0) {
          accepted_.push_back(client_fd);
        }
      }
    }
  }

  std::atomic<bool> stop_{false};
  int listen_fd_{-1};
  int port_{0};
  std::thread accept_thread_{};
  std::vector<int> accepted_{};
};

}  // namespace

TEST(WebsocketColdPathLoopTest, TotalBudgetTimeoutDuringTlsHandshake) {
  BlackholeTcpServer server;
  ASSERT_TRUE(server.Start());

  ConnectionConfig config{};
  config.host = "127.0.0.1";
  config.service = std::to_string(server.port());
  config.target = "/";
  config.enable_tls = true;
  config.cold_path_total_timeout_ms = 200;

  TlsSocket socket;
  StateMachine state_machine;
  ColdPathLoop loop;
  std::array<char, 4096> storage{};

  const auto start = std::chrono::steady_clock::now();
  const bool ok = loop.RunUntilActive(socket, state_machine, config, storage);
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();

  EXPECT_FALSE(ok);
  EXPECT_EQ(state_machine.last_error(), ConnectionError::kConnectTimeout);
  EXPECT_EQ(state_machine.phase(), ConnectionPhase::kTlsHandshaking);
  EXPECT_GE(elapsed_ms, 150);
  EXPECT_LT(elapsed_ms, 2000);
}
