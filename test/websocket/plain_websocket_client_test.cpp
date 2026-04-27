#include "core/websocket/websocket_client.h"

#include <fmt/format.h>
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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/websocket/handshake.h"

using namespace aquila::websocket;
using namespace std::chrono_literals;

namespace {

class PlainWebSocketServer {
 public:
  explicit PlainWebSocketServer(
      std::string payload,
      bool coalesce_first_frame_with_handshake = false)
      : payload_(std::move(payload)),
        coalesce_first_frame_with_handshake_(
            coalesce_first_frame_with_handshake) {}

  ~PlainWebSocketServer() noexcept {
    stop_.store(true, std::memory_order_release);
    const int active_fd = active_client_fd_.exchange(-1);
    if (active_fd >= 0) {
      ::shutdown(active_fd, SHUT_RDWR);
      ::close(active_fd);
    }
    if (thread_.joinable()) {
      thread_.join();
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
    thread_ = std::thread([this] { Run(); });
    return true;
  }

  int port() const noexcept { return port_; }

  size_t sent_frame_count() const noexcept {
    return sent_frame_count_.load(std::memory_order_acquire);
  }

 private:
  void Run() noexcept {
    while (!stop_.load(std::memory_order_acquire)) {
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(listen_fd_, &read_set);
      timeval tv{0, 20'000};
      const int ready =
          ::select(listen_fd_ + 1, &read_set, nullptr, nullptr, &tv);
      if (ready <= 0) {
        continue;
      }

      const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
      if (client_fd < 0) {
        continue;
      }
      active_client_fd_.store(client_fd, std::memory_order_release);
      HandleClient(client_fd);
      active_client_fd_.store(-1, std::memory_order_release);
      ::close(client_fd);
    }
  }

  void HandleClient(int client_fd) noexcept {
    timeval timeout{2, 0};
    ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout));
    ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                 sizeof(timeout));

    std::string request;
    char buffer[512];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const ssize_t received = ::recv(client_fd, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        return;
      }
      request.append(buffer, static_cast<size_t>(received));
      if (request.size() > 4096) {
        return;
      }
    }

    const std::string_view key = FindHeader(request, "Sec-WebSocket-Key");
    if (key.empty()) {
      return;
    }
    std::array<char, 64> accept_storage{};
    std::string_view accept_key{};
    if (!detail::ComputeAcceptKey(key, accept_storage, accept_key)) {
      return;
    }

    const std::string response =
        fmt::format("HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: {}\r\n"
                    "\r\n",
                    accept_key);
    const auto frame = BuildTextFrame();
    if (coalesce_first_frame_with_handshake_) {
      std::vector<std::byte> combined;
      combined.reserve(response.size() + frame.size());
      const auto response_bytes = std::as_bytes(std::span(response));
      combined.insert(combined.end(), response_bytes.begin(),
                      response_bytes.end());
      combined.insert(combined.end(), frame.begin(), frame.end());
      if (!SendAll(client_fd, combined)) {
        return;
      }
      sent_frame_count_.fetch_add(1, std::memory_order_acq_rel);
    } else {
      if (!SendAll(client_fd, std::as_bytes(std::span(response)))) {
        return;
      }

      std::this_thread::sleep_for(20ms);
      if (!SendAll(client_fd, frame)) {
        return;
      }
      sent_frame_count_.fetch_add(1, std::memory_order_acq_rel);
    }

    while (!stop_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(5ms);
    }
  }

  static bool SendAll(int fd, std::span<const std::byte> bytes) noexcept {
    size_t offset = 0;
    while (offset < bytes.size()) {
      const ssize_t written =
          ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
      if (written <= 0) {
        return false;
      }
      offset += static_cast<size_t>(written);
    }
    return true;
  }

  static std::string_view FindHeader(std::string_view request,
                                     std::string_view header) noexcept {
    size_t line_begin = 0;
    while (line_begin < request.size()) {
      const size_t line_end = request.find("\r\n", line_begin);
      if (line_end == std::string_view::npos || line_end == line_begin) {
        return {};
      }
      const std::string_view line =
          request.substr(line_begin, line_end - line_begin);
      const size_t colon = line.find(':');
      if (colon != std::string_view::npos) {
        const auto name = detail::TrimAsciiWhitespace(line.substr(0, colon));
        if (detail::AsciiEqualsIgnoreCase(name, header)) {
          return detail::TrimAsciiWhitespace(line.substr(colon + 1));
        }
      }
      line_begin = line_end + 2;
    }
    return {};
  }

  std::vector<std::byte> BuildTextFrame() const {
    std::vector<std::byte> frame(2 + payload_.size());
    frame[0] = std::byte{0x81};
    frame[1] = std::byte{static_cast<unsigned char>(payload_.size())};
    for (size_t i = 0; i < payload_.size(); ++i) {
      frame[2 + i] = std::byte{static_cast<unsigned char>(payload_[i])};
    }
    return frame;
  }

  std::string payload_;
  bool coalesce_first_frame_with_handshake_{false};
  std::atomic<bool> stop_{false};
  std::atomic<int> active_client_fd_{-1};
  std::atomic<size_t> sent_frame_count_{0};
  int listen_fd_{-1};
  int port_{0};
  std::thread thread_;
};

struct MessageCapture {
  void Push(std::string_view payload) {
    std::lock_guard lock(mutex);
    messages.emplace_back(payload);
    cv.notify_all();
  }

  bool WaitForMessage(std::string_view expected,
                      std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, timeout, [&] {
      return !messages.empty() && messages.back() == expected;
    });
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> messages;
};

struct StateCapture {
  void Push(ConnectionPhase phase) {
    std::lock_guard lock(mutex);
    phases.push_back(phase);
    cv.notify_all();
  }

  bool WaitFor(ConnectionPhase phase, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, timeout, [&] {
      for (const auto candidate : phases) {
        if (candidate == phase) {
          return true;
        }
      }
      return false;
    });
  }

  bool Contains(ConnectionPhase phase) const {
    std::lock_guard lock(mutex);
    for (const auto candidate : phases) {
      if (candidate == phase) {
        return true;
      }
    }
    return false;
  }

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::vector<ConnectionPhase> phases;
};

DeliveryResult CaptureMessage(void* context, const MessageView& view) noexcept {
  auto* capture = static_cast<MessageCapture*>(context);
  const auto payload = std::string_view(
      reinterpret_cast<const char*>(view.payload.data()), view.payload.size());
  capture->Push(payload);
  return DeliveryResult::kAccepted;
}

void CaptureState(void* context, ConnectionPhase phase) noexcept {
  static_cast<StateCapture*>(context)->Push(phase);
}

}  // namespace

TEST(PlainWebSocketClientTest, ConnectsWithoutTlsAndReceivesMessage) {
  PlainWebSocketServer server("plain-ok");
  ASSERT_TRUE(server.Start());

  ConnectionConfig config{};
  config.host = "127.0.0.1";
  config.service = fmt::format("{}", server.port());
  config.target = "/";
  config.enable_tls = false;
  config.heartbeat_interval_ms = 60'000;
  config.heartbeat_timeout_ms = 60'000;
  config.runtime_policy.active_spin = false;

  MessageCapture messages;
  StateCapture states;
  MessageConsumer consumer{&messages, &CaptureMessage};
  PlainWebSocketClient client(config, consumer);
  client.SetStateHandler(&states, &CaptureState);

  bool result = false;
  std::thread io_thread([&] { result = client.Start(); });

  const bool entered_active = states.WaitFor(ConnectionPhase::kActive, 2s);
  const bool received_message = messages.WaitForMessage("plain-ok", 2s);
  client.Stop();
  io_thread.join();

  EXPECT_TRUE(entered_active);
  EXPECT_EQ(server.sent_frame_count(), 1U);
  EXPECT_TRUE(received_message);
  EXPECT_TRUE(result);
  EXPECT_FALSE(states.Contains(ConnectionPhase::kTlsHandshaking));
}

TEST(PlainWebSocketClientTest, ReceivesFrameCoalescedWithHandshakeResponse) {
  PlainWebSocketServer server("coalesced-ok", true);
  ASSERT_TRUE(server.Start());

  ConnectionConfig config{};
  config.host = "127.0.0.1";
  config.service = fmt::format("{}", server.port());
  config.target = "/";
  config.enable_tls = false;
  config.heartbeat_interval_ms = 60'000;
  config.heartbeat_timeout_ms = 60'000;
  config.runtime_policy.active_spin = false;

  MessageCapture messages;
  StateCapture states;
  MessageConsumer consumer{&messages, &CaptureMessage};
  PlainWebSocketClient client(config, consumer);
  client.SetStateHandler(&states, &CaptureState);

  bool result = false;
  std::thread io_thread([&] { result = client.Start(); });

  const bool entered_active = states.WaitFor(ConnectionPhase::kActive, 2s);
  const bool received_message = messages.WaitForMessage("coalesced-ok", 2s);
  client.Stop();
  io_thread.join();

  EXPECT_TRUE(entered_active);
  EXPECT_EQ(server.sent_frame_count(), 1U);
  EXPECT_TRUE(received_message);
  EXPECT_TRUE(result);
  EXPECT_FALSE(states.Contains(ConnectionPhase::kTlsHandshaking));
}
