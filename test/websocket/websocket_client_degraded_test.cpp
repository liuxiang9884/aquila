#include "core/websocket/websocket_client.h"
#include "test/websocket/tls_blackhole_server.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
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

ConnectionConfig BuildDegradedConfig(int port) {
  ConnectionConfig config{};
  config.host = "localhost";
  config.service = fmt::format("{}", port);
  config.target = "/";
  config.enable_tls = true;
  config.cold_path_total_timeout_ms = 2'000;
  config.heartbeat_interval_ms = 5;
  config.heartbeat_timeout_ms = 80;
  config.runtime_policy.active_spin = false;
  config.runtime_policy.spin_iterations_before_clock_check = 1;
  config.reconnect.initial_backoff_ms = 1'000;
  config.reconnect.max_backoff_ms = 1'000;
  config.reconnect.jitter_percent = 0;
  config.degraded.high_watermark_percent = 0;
  config.degraded.backpressure_drops_per_second = 0;
  config.degraded.awaiting_pong_timeout_ms = 20;
  config.degraded.recover_ticks = 2;
  config.degraded.evaluation_interval_iterations = 1;
  return config;
}

}  // namespace

TEST(WebSocketClientDegradedTest,
     MissingPongTransitionsThroughDegradedBeforeReconnectBackoff) {
  test::TlsBlackholeServer server({
      test::TlsServerAction::kHandshake101ThenStayOpen,
  });
  ASSERT_TRUE(server.Start());

  StateCapture states;
  MessageConsumer consumer{nullptr, &AcceptAll};
  WebSocketClient client(BuildDegradedConfig(server.port()), consumer);
  client.SetStateHandler(&states, &RecordState);

  std::atomic<bool> done{false};
  bool result = false;
  std::thread io_thread([&] {
    result = client.Start();
    done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(states.WaitFor(ConnectionPhase::kDegraded, 2s));
  auto degraded_metrics = client.SnapshotMetrics();
  EXPECT_EQ(degraded_metrics.degraded_active, 1U);
  EXPECT_EQ(degraded_metrics.degraded_enter_count, 1U);
  EXPECT_EQ(degraded_metrics.degraded_exit_count, 0U);

  ASSERT_TRUE(states.WaitFor(ConnectionPhase::kReconnectBackoff, 2s));
  client.Stop();
  io_thread.join();

  EXPECT_TRUE(result);
  EXPECT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_TRUE(states.ContainsSubsequence({
      ConnectionPhase::kActive,
      ConnectionPhase::kDegraded,
      ConnectionPhase::kReconnectBackoff,
  }));
  const auto final_metrics = client.SnapshotMetrics();
  EXPECT_EQ(final_metrics.degraded_active, 0U);
  EXPECT_EQ(final_metrics.degraded_exit_count, 1U);
}
