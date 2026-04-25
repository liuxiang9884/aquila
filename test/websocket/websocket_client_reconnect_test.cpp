#include "core/websocket/websocket_client.h"
#include "test/websocket/tls_blackhole_server.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

using namespace aquila::websocket;
using namespace std::chrono_literals;

namespace {

DeliveryResult AcceptAll(void*, const MessageView&) noexcept {
  return DeliveryResult::kAccepted;
}

struct StateCapture {
  void Push(ConnectionPhase phase) {
    std::lock_guard lock(mutex);
    phases.push_back(phase);
    cv.notify_all();
  }

  bool WaitForCount(ConnectionPhase phase, size_t count,
                    std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, timeout, [&] {
      size_t seen = 0;
      for (const auto candidate : phases) {
        if (candidate == phase) {
          ++seen;
        }
      }
      return seen >= count;
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

  bool ContainsSubsequence(std::initializer_list<ConnectionPhase> expected)
      const {
    std::lock_guard lock(mutex);
    auto cursor = expected.begin();
    for (const auto phase : phases) {
      if (cursor != expected.end() && phase == *cursor) {
        ++cursor;
      }
    }
    return cursor == expected.end();
  }

  mutable std::mutex mutex{};
  std::condition_variable cv{};
  std::vector<ConnectionPhase> phases{};
};

void RecordState(void* context, ConnectionPhase phase) noexcept {
  static_cast<StateCapture*>(context)->Push(phase);
}

ConnectionConfig BuildReconnectConfig(int port) {
  ConnectionConfig config{};
  config.host = "localhost";
  config.service = fmt::format("{}", port);
  config.target = "/";
  config.enable_tls = true;
  config.cold_path_total_timeout_ms = 2'000;
  config.heartbeat_interval_ms = 60'000;
  config.heartbeat_timeout_ms = 60'000;
  config.runtime_policy.active_spin = false;
  config.runtime_policy.spin_iterations_before_clock_check = 64;
  config.reconnect.initial_backoff_ms = 10;
  config.reconnect.max_backoff_ms = 80;
  config.reconnect.backoff_shift_bits = 1;
  config.reconnect.jitter_percent = 0;
  return config;
}

}  // namespace

TEST(WebSocketClientReconnectTest, ReconnectsAfterActivePeerClose) {
  test::TlsBlackholeServer server({
      test::TlsServerAction::kHandshake101ThenClose,
      test::TlsServerAction::kHandshake101ThenClose,
      test::TlsServerAction::kHandshake101ThenStayOpen,
  });
  ASSERT_TRUE(server.Start());

  StateCapture states;
  MessageConsumer consumer{nullptr, &AcceptAll};
  WebSocketClient client(BuildReconnectConfig(server.port()), consumer);
  client.SetStateHandler(&states, &RecordState);

  std::atomic<bool> done{false};
  bool result = false;
  std::thread io_thread([&] {
    result = client.Start();
    done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(states.WaitForCount(ConnectionPhase::kActive, 3, 5s));
  client.Stop();
  io_thread.join();

  EXPECT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_TRUE(result);
  EXPECT_GE(client.SnapshotMetrics().reconnects, 2U);
  EXPECT_TRUE(states.ContainsSubsequence({
      ConnectionPhase::kActive,
      ConnectionPhase::kReconnectBackoff,
      ConnectionPhase::kActive,
  }));
}

TEST(WebSocketClientReconnectTest, RetriesTransientFailuresWithBackoff) {
  test::TlsBlackholeServer server({
      test::TlsServerAction::kCloseTcpImmediately,
      test::TlsServerAction::kCloseTcpImmediately,
      test::TlsServerAction::kHandshake101ThenStayOpen,
  });
  ASSERT_TRUE(server.Start());

  StateCapture states;
  MessageConsumer consumer{nullptr, &AcceptAll};
  WebSocketClient client(BuildReconnectConfig(server.port()), consumer);
  client.SetStateHandler(&states, &RecordState);

  const auto start = std::chrono::steady_clock::now();
  std::atomic<bool> done{false};
  bool result = false;
  std::thread io_thread([&] {
    result = client.Start();
    done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(states.WaitForCount(ConnectionPhase::kReconnectBackoff, 2, 5s));
  ASSERT_TRUE(states.WaitForCount(ConnectionPhase::kActive, 1, 5s));
  client.Stop();
  io_thread.join();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  EXPECT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_TRUE(result);
  EXPECT_GE(elapsed_ms, 25ms);
  EXPECT_GE(client.SnapshotMetrics().reconnects, 1U);
}

TEST(WebSocketClientReconnectTest, StopsAfterMaxAttempts) {
  test::TlsBlackholeServer server({
      test::TlsServerAction::kCloseTcpImmediately,
      test::TlsServerAction::kCloseTcpImmediately,
      test::TlsServerAction::kCloseTcpImmediately,
  });
  ASSERT_TRUE(server.Start());

  StateCapture states;
  MessageConsumer consumer{nullptr, &AcceptAll};
  auto config = BuildReconnectConfig(server.port());
  config.reconnect.max_attempts = 3;
  config.reconnect.initial_backoff_ms = 1;
  config.reconnect.max_backoff_ms = 4;
  WebSocketClient client(config, consumer);
  client.SetStateHandler(&states, &RecordState);

  EXPECT_FALSE(client.Start());
  EXPECT_TRUE(states.Contains(ConnectionPhase::kClosed));
  EXPECT_EQ(client.SnapshotMetrics().reconnects, 0U);
  EXPECT_GE(server.accepted_count(), 3U);
}

TEST(WebSocketClientReconnectTest, StopInterruptsReconnectBackoff) {
  test::TlsBlackholeServer server({
      test::TlsServerAction::kCloseTcpImmediately,
  });
  ASSERT_TRUE(server.Start());

  StateCapture states;
  MessageConsumer consumer{nullptr, &AcceptAll};
  auto config = BuildReconnectConfig(server.port());
  config.reconnect.initial_backoff_ms = 5'000;
  config.reconnect.max_backoff_ms = 5'000;
  WebSocketClient client(config, consumer);
  client.SetStateHandler(&states, &RecordState);

  std::atomic<bool> done{false};
  bool result = false;
  std::thread io_thread([&] {
    result = client.Start();
    done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(states.WaitForCount(ConnectionPhase::kReconnectBackoff, 1, 1s));
  const auto stop_start = std::chrono::steady_clock::now();
  client.Stop();
  for (int i = 0; i < 100 &&
                  !done.load(std::memory_order_acquire);
       ++i) {
    std::this_thread::sleep_for(5ms);
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - stop_start);
  io_thread.join();

  EXPECT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_TRUE(result);
  EXPECT_LT(elapsed_ms, 1s);
  EXPECT_TRUE(states.Contains(ConnectionPhase::kClosing));
  EXPECT_EQ(client.SnapshotMetrics().reconnects, 0U);
}

TEST(WebSocketClientReconnectTest, StopInterruptsColdPathTlsHandshake) {
  test::TlsBlackholeServer server({
      test::TlsServerAction::kBlackholeTcp,
  });
  ASSERT_TRUE(server.Start());

  StateCapture states;
  MessageConsumer consumer{nullptr, &AcceptAll};
  auto config = BuildReconnectConfig(server.port());
  config.cold_path_total_timeout_ms = 5'000;
  WebSocketClient client(config, consumer);
  client.SetStateHandler(&states, &RecordState);

  std::atomic<bool> done{false};
  bool result = false;
  std::thread io_thread([&] {
    result = client.Start();
    done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(server.WaitForAccepted(1, 1s));
  const auto stop_start = std::chrono::steady_clock::now();
  client.Stop();
  for (int i = 0; i < 100 &&
                  !done.load(std::memory_order_acquire);
       ++i) {
    std::this_thread::sleep_for(5ms);
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - stop_start);
  io_thread.join();

  EXPECT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_TRUE(result);
  EXPECT_LT(elapsed_ms, 1s);
  EXPECT_TRUE(states.Contains(ConnectionPhase::kClosing));
  EXPECT_EQ(client.SnapshotMetrics().reconnects, 0U);
}
