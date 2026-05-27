#include "core/websocket/plain_socket.h"

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>

namespace aquila::websocket {
namespace {

class LoopbackTcpServer {
 public:
  ~LoopbackTcpServer() noexcept {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
      thread_.join();
    }
    if (accepted_fd_ >= 0) {
      ::close(accepted_fd_);
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
    if (::listen(listen_fd_, 1) != 0) {
      return false;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) !=
        0) {
      return false;
    }
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { AcceptLoop(); });
    return true;
  }

  int port() const noexcept {
    return port_;
  }

 private:
  void AcceptLoop() noexcept {
    while (!stop_.load(std::memory_order_relaxed)) {
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(listen_fd_, &read_set);
      timeval tv{0, 50'000};
      const int ready =
          ::select(listen_fd_ + 1, &read_set, nullptr, nullptr, &tv);
      if (ready <= 0) {
        continue;
      }
      accepted_fd_ = ::accept(listen_fd_, nullptr, nullptr);
      return;
    }
  }

  std::atomic<bool> stop_{false};
  int listen_fd_{-1};
  int accepted_fd_{-1};
  int port_{0};
  std::thread thread_{};
};

bool FinishConnect(PlainSocket& socket) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < deadline) {
    if (socket.FinishHandshake()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

TEST(WebsocketPlainSocketTest, ConnectIpOverridesTcpConnectHost) {
  LoopbackTcpServer server;
  ASSERT_TRUE(server.Start());

  ConnectionConfig config{};
  config.host = "logical.invalid";
  config.connect_ip = "127.0.0.1";
  config.port = std::to_string(server.port());

  PlainSocket socket;
  ASSERT_TRUE(socket.OpenAndConnect(config));
  EXPECT_TRUE(FinishConnect(socket));
}

}  // namespace
}  // namespace aquila::websocket
