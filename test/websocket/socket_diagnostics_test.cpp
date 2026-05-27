#include "core/websocket/socket_diagnostics.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>

namespace aquila::websocket {
namespace {

class Fd {
 public:
  explicit Fd(int fd = -1) noexcept : fd_(fd) {}
  ~Fd() noexcept {
    Reset();
  }

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  Fd(Fd&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      Reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept {
    return fd_;
  }

  void Reset() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_{-1};
};

struct TcpPair {
  Fd client;
  Fd server;
};

TcpPair MakeLoopbackTcpPair() {
  Fd listener(::socket(AF_INET, SOCK_STREAM, 0));
  EXPECT_GE(listener.get(), 0);

  sockaddr_in listen_addr{};
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  listen_addr.sin_port = 0;
  EXPECT_EQ(::bind(listener.get(), reinterpret_cast<sockaddr*>(&listen_addr),
                   sizeof(listen_addr)),
            0);
  EXPECT_EQ(::listen(listener.get(), 1), 0);

  socklen_t listen_len = sizeof(listen_addr);
  EXPECT_EQ(
      ::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&listen_addr),
                    &listen_len),
      0);

  Fd client(::socket(AF_INET, SOCK_STREAM, 0));
  EXPECT_GE(client.get(), 0);
  EXPECT_EQ(::connect(client.get(), reinterpret_cast<sockaddr*>(&listen_addr),
                      sizeof(listen_addr)),
            0);

  Fd server(::accept(listener.get(), nullptr, nullptr));
  EXPECT_GE(server.get(), 0);
  return TcpPair{.client = std::move(client), .server = std::move(server)};
}

TEST(WebsocketSocketDiagnosticsTest, InvalidFdSnapshotsAreUnavailable) {
  const SocketEndpointDiagnostics endpoints =
      SnapshotSocketEndpointDiagnostics(-1);
  EXPECT_FALSE(endpoints.available);
  EXPECT_EQ(std::string_view(endpoints.local_ip.data()), "");
  EXPECT_EQ(endpoints.local_port, 0U);
  EXPECT_EQ(std::string_view(endpoints.remote_ip.data()), "");
  EXPECT_EQ(endpoints.remote_port, 0U);

  const TcpInfoDiagnostics tcp_info = SnapshotTcpInfoDiagnostics(-1);
  EXPECT_FALSE(tcp_info.available);
  EXPECT_EQ(tcp_info.rtt_us, 0U);
  EXPECT_EQ(tcp_info.total_retrans, 0U);
}

TEST(WebsocketSocketDiagnosticsTest, CapturesLoopbackEndpointsAndTcpInfo) {
  TcpPair pair = MakeLoopbackTcpPair();

  const SocketEndpointDiagnostics client_endpoints =
      SnapshotSocketEndpointDiagnostics(pair.client.get());
  const SocketEndpointDiagnostics server_endpoints =
      SnapshotSocketEndpointDiagnostics(pair.server.get());

  ASSERT_TRUE(client_endpoints.available);
  ASSERT_TRUE(server_endpoints.available);
  EXPECT_EQ(std::string_view(client_endpoints.local_ip.data()), "127.0.0.1");
  EXPECT_EQ(std::string_view(client_endpoints.remote_ip.data()), "127.0.0.1");
  EXPECT_EQ(std::string_view(server_endpoints.local_ip.data()), "127.0.0.1");
  EXPECT_EQ(std::string_view(server_endpoints.remote_ip.data()), "127.0.0.1");
  EXPECT_NE(client_endpoints.local_port, 0U);
  EXPECT_NE(client_endpoints.remote_port, 0U);
  EXPECT_EQ(client_endpoints.local_port, server_endpoints.remote_port);
  EXPECT_EQ(client_endpoints.remote_port, server_endpoints.local_port);

  const TcpInfoDiagnostics tcp_info =
      SnapshotTcpInfoDiagnostics(pair.client.get());
#if defined(__linux__)
  EXPECT_TRUE(tcp_info.available);
#else
  EXPECT_FALSE(tcp_info.available);
#endif
}

}  // namespace
}  // namespace aquila::websocket
