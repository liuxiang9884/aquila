#include "core/websocket/socket_timestamping.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <fcntl.h>

namespace aquila::websocket {
namespace {

#if !AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
#define AQUILA_SKIP_SOCKET_TIMESTAMPING_ATTRIBUTION_TEST() \
  GTEST_SKIP() << "socket timestamping attribution is disabled at build time"
#else
#define AQUILA_SKIP_SOCKET_TIMESTAMPING_ATTRIBUTION_TEST() \
  do {                                                     \
  } while (false)
#endif

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

struct LoopbackTcpPair {
  Fd client;
  Fd server;
};

LoopbackTcpPair CreateLoopbackTcpPairForTest() {
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

  const int flags = ::fcntl(client.get(), F_GETFL, 0);
  EXPECT_GE(flags, 0);
  EXPECT_EQ(::fcntl(client.get(), F_SETFL, flags | O_NONBLOCK), 0);

  return LoopbackTcpPair{.client = std::move(client),
                         .server = std::move(server)};
}

void RecvExactForTest(int fd, size_t bytes) {
  std::array<char, 64> buffer{};
  size_t received_bytes = 0;
  while (received_bytes < bytes) {
    const size_t remaining = bytes - received_bytes;
    const ssize_t received =
        ::recv(fd, buffer.data(), std::min(buffer.size(), remaining), 0);
    ASSERT_GT(received, 0);
    received_bytes += static_cast<size_t>(received);
  }
}

TEST(WebsocketSocketTimestampingTest, DefaultConfigIsDisabled) {
  SocketTimestampingConfig config;

  EXPECT_FALSE(config.enabled);
  EXPECT_FALSE(config.tx_software);
  EXPECT_FALSE(config.tx_sched);
  EXPECT_FALSE(config.tx_ack);
  EXPECT_FALSE(config.rx_software);
}

TEST(WebsocketSocketTimestampingTest, ComputesLocalStageDurations) {
  SocketTimestampingSnapshot snapshot;
  snapshot.write_complete_ns = 100;
  snapshot.tx_software_ns = 140;
  snapshot.tx_ack_ns = 250;
  snapshot.rx_software_ns = 400;
  snapshot.ack_receive_local_ns = 430;

  const SocketTimestampingStages stages =
      ComputeSocketTimestampingStages(snapshot);

  EXPECT_EQ(stages.write_complete_to_tx_software_ns, 40);
  EXPECT_EQ(stages.tx_software_to_tx_ack_ns, 110);
  EXPECT_EQ(stages.tx_ack_to_rx_software_ns, 150);
  EXPECT_EQ(stages.rx_software_to_ack_receive_ns, 30);
}

TEST(WebsocketSocketTimestampingTest, ApplyDisabledConfigSucceedsOnInvalidFd) {
  SocketTimestampingConfig config;

  const SocketTimestampingApplyResult result =
      ApplySocketTimestampingConfig(-1, config);

  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.enabled);
}

TEST(WebsocketSocketTimestampingTest,
     ApplyEnabledConfigReportsFailureOnInvalidFd) {
  SocketTimestampingConfig config;
  config.enabled = true;
  config.tx_software = true;
  config.rx_software = true;

  const SocketTimestampingApplyResult result =
      ApplySocketTimestampingConfig(-1, config);

#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.enabled);
  EXPECT_NE(result.error_errno, 0);
#else
  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.enabled);
  EXPECT_EQ(result.error_errno, 0);
#endif
}

TEST(WebsocketSocketTimestampingTest, DrainInvalidFdReportsFailure) {
  const SocketTimestampingEventDrain drain =
      DrainSocketTimestampingErrorQueue(-1, 16);

#if AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
  EXPECT_FALSE(drain.ok);
  EXPECT_NE(drain.error_errno, 0);
  EXPECT_EQ(drain.events_seen, 0U);
#else
  EXPECT_TRUE(drain.ok);
  EXPECT_EQ(drain.error_errno, 0);
  EXPECT_EQ(drain.events_seen, 0U);
#endif
}

TEST(WebsocketSocketTimestampingTest, LoopbackTxTimestampDrainIsNonBlocking) {
  AQUILA_SKIP_SOCKET_TIMESTAMPING_ATTRIBUTION_TEST();
  SocketTimestampingConfig config;
  config.enabled = true;
  config.tx_software = true;
  config.tx_ack = true;
  config.max_errqueue_events_per_drain = 16;

  LoopbackTcpPair pair = CreateLoopbackTcpPairForTest();
  const SocketTimestampingApplyResult apply =
      ApplySocketTimestampingConfig(pair.client.get(), config);
  if (!apply.ok) {
    GTEST_SKIP() << "SO_TIMESTAMPING unavailable errno=" << apply.error_errno;
  }

  ASSERT_EQ(::send(pair.client.get(), "x", 1, MSG_NOSIGNAL), 1);
  char byte = 0;
  ASSERT_EQ(::recv(pair.server.get(), &byte, 1, 0), 1);

  const SocketTimestampingEventDrain drain =
      DrainSocketTimestampingErrorQueue(pair.client.get(), 16);

  EXPECT_TRUE(drain.ok);
  EXPECT_LE(drain.events_seen, 16U);
}

TEST(WebsocketSocketTimestampingTest, LoopbackTcpOptIdReportsLastByteIds) {
  AQUILA_SKIP_SOCKET_TIMESTAMPING_ATTRIBUTION_TEST();
#if defined(__linux__) && defined(SOF_TIMESTAMPING_OPT_ID_TCP)
  SocketTimestampingConfig config;
  config.enabled = true;
  config.tx_software = true;
  config.tx_ack = true;
  config.max_errqueue_events_per_drain = 32;

  LoopbackTcpPair pair = CreateLoopbackTcpPairForTest();
  int one = 1;
  EXPECT_EQ(::setsockopt(pair.client.get(), IPPROTO_TCP, TCP_NODELAY, &one,
                         sizeof(one)),
            0);

  const SocketTimestampingApplyResult apply =
      ApplySocketTimestampingConfig(pair.client.get(), config);
  if (!apply.ok) {
    GTEST_SKIP() << "SO_TIMESTAMPING unavailable errno=" << apply.error_errno;
  }

  ASSERT_EQ(::send(pair.client.get(), "abc", 3, MSG_NOSIGNAL), 3);
  RecvExactForTest(pair.server.get(), 3);
  ASSERT_EQ(::send(pair.client.get(), "defgh", 5, MSG_NOSIGNAL), 5);
  RecvExactForTest(pair.server.get(), 5);

  std::vector<SocketTimestampingEvent> events;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const SocketTimestampingEventDrain drain =
        DrainSocketTimestampingErrorQueue(pair.client.get(), 32);
    ASSERT_TRUE(drain.ok) << "errno=" << drain.error_errno;
    for (std::uint32_t i = 0; i < drain.events_seen; ++i) {
      events.push_back(drain.events[i]);
    }
    const auto has_last_byte_id = [&events](std::uint32_t id) {
      return std::any_of(
          events.begin(), events.end(),
          [id](const SocketTimestampingEvent& event) {
            return (event.kind == SocketTimestampingEventKind::kTxSoftware ||
                    event.kind == SocketTimestampingEventKind::kTxAck) &&
                   event.id == id;
          });
    };
    if (has_last_byte_id(2) && has_last_byte_id(7)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const auto has_last_byte_id = [&events](std::uint32_t id) {
    return std::any_of(
        events.begin(), events.end(),
        [id](const SocketTimestampingEvent& event) {
          return (event.kind == SocketTimestampingEventKind::kTxSoftware ||
                  event.kind == SocketTimestampingEventKind::kTxAck) &&
                 event.id == id;
        });
  };
  EXPECT_TRUE(has_last_byte_id(2));
  EXPECT_TRUE(has_last_byte_id(7));
#else
  GTEST_SKIP() << "SOF_TIMESTAMPING_OPT_ID_TCP is unavailable";
#endif
}

}  // namespace
}  // namespace aquila::websocket
