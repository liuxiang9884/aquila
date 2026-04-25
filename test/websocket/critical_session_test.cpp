#include "core/websocket/critical_session.h"
#include "core/websocket/cold_path_loop.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

using namespace aquila::websocket;

namespace {

DeliveryResult RecordMessage(void* context, const MessageView& view) noexcept {
  auto* bytes = static_cast<size_t*>(context);
  *bytes += view.payload.size();
  return DeliveryResult::kAccepted;
}

std::vector<std::byte> BuildServerTextFrame(std::string_view payload) {
  std::vector<std::byte> frame(2 + payload.size());
  frame[0] = std::byte{0x81};
  frame[1] = std::byte{static_cast<unsigned char>(payload.size())};
  for (size_t i = 0; i < payload.size(); ++i) {
    frame[2 + i] = static_cast<std::byte>(payload[i]);
  }
  return frame;
}

std::vector<std::byte> BuildServerPingFrame(std::string_view payload) {
  std::vector<std::byte> frame(2 + payload.size());
  frame[0] = std::byte{0x89};
  frame[1] = std::byte{static_cast<unsigned char>(payload.size())};
  for (size_t i = 0; i < payload.size(); ++i) {
    frame[2 + i] = static_cast<std::byte>(payload[i]);
  }
  return frame;
}

class FakeTlsSocket final {
 public:
  ssize_t ReadSome(std::span<std::byte> buffer) noexcept {
    if (read_chunks_.empty()) {
      if (eof_on_empty_) {
        eof_on_empty_ = false;
        return 0;
      }
      errno = EAGAIN;
      return -1;
    }
    if (buffer.empty()) {
      errno = EAGAIN;
      return -1;
    }
    auto& chunk = read_chunks_.front();
    const size_t bytes = std::min(buffer.size(), chunk.size());
    std::copy_n(chunk.begin(), bytes, buffer.begin());
    if (bytes == chunk.size()) {
      read_chunks_.erase(read_chunks_.begin());
    } else {
      chunk.erase(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(bytes));
    }
    return static_cast<ssize_t>(bytes);
  }

  ssize_t WriteSome(std::span<const std::byte> buffer) noexcept {
    if (pending_write_eagain_) {
      pending_write_eagain_ = false;
      errno = EAGAIN;
      return -1;
    }

    const size_t chunk = max_write_bytes_per_call_ == 0
                             ? buffer.size()
                             : std::min(buffer.size(), max_write_bytes_per_call_);
    written_.insert(written_.end(), buffer.begin(), buffer.begin() + chunk);
    if (chunk < buffer.size() && eagain_after_partial_write_) {
      pending_write_eagain_ = true;
    }
    return static_cast<ssize_t>(chunk);
  }

  size_t max_write_bytes_per_call_{0};
  bool eagain_after_partial_write_{true};
  bool pending_write_eagain_{false};
  bool eof_on_empty_{false};
  std::vector<std::vector<std::byte>> read_chunks_{};
  std::vector<std::byte> written_{};
};

}  // namespace

TEST(WebsocketCriticalSessionTest,
     HandlesWritesReadsBackpressureAndHeartbeatFailures) {
  ConnectionConfig config{};
  config.prepared_write_slots = 4;
  config.prepared_write_bytes = 128;
  config.read_buffer_bytes = 256;
  config.frame_buffer_bytes = 256;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.max_write_bytes_per_call_ = 2;

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);

  auto* write = session.TryAcquirePreparedWrite();
  ASSERT_NE(write, nullptr);
  std::copy_n(std::as_bytes(std::span{"tick", 4}).begin(), 4,
              write->storage.begin());
  write->encoded_size = 4;
  write->kind = PayloadKind::kText;
  ASSERT_EQ(session.CommitPreparedWrite(write), SendStatus::kOk);

  session.DriveWrite();
  EXPECT_EQ(socket.written_.size(), 2U);
  EXPECT_EQ(metrics.tx_messages, 0U);
  EXPECT_EQ(metrics.tx_bytes, 2U);
  EXPECT_TRUE(session.WantsWrite());

  session.DriveWrite();
  EXPECT_EQ(socket.written_.size(), 4U);
  EXPECT_EQ(metrics.tx_messages, 1U);
  EXPECT_EQ(metrics.tx_bytes, 4U);
  EXPECT_FALSE(session.WantsWrite());

  auto* cancelled = session.TryAcquirePreparedWrite();
  ASSERT_NE(cancelled, nullptr);
  session.CancelPreparedWrite(cancelled);
  auto* reacquired = session.TryAcquirePreparedWrite();
  ASSERT_NE(reacquired, nullptr);
  session.CancelPreparedWrite(reacquired);

  auto first_frame = BuildServerTextFrame("ab");
  auto second_frame = BuildServerTextFrame("cd");
  std::vector<std::byte> coalesced;
  coalesced.reserve(first_frame.size() + second_frame.size());
  coalesced.insert(coalesced.end(), first_frame.begin(), first_frame.end());
  coalesced.insert(coalesced.end(), second_frame.begin(), second_frame.end());
  socket.read_chunks_.push_back(coalesced);
  session.DriveRead();
  EXPECT_EQ(bytes, 4U);
  EXPECT_EQ(metrics.rx_messages, 2U);
  EXPECT_EQ(metrics.rx_bytes, coalesced.size());

  const size_t written_before_ping = socket.written_.size();
  socket.max_write_bytes_per_call_ = 0;
  socket.read_chunks_.push_back(BuildServerPingFrame("z"));
  session.DriveRead();
  EXPECT_TRUE(session.WantsWrite());
  session.DriveWrite();
  EXPECT_GT(socket.written_.size(), written_before_ping);

  FakeTlsSocket peer_closed_socket;
  peer_closed_socket.eof_on_empty_ = true;
  CriticalSession<FakeTlsSocket> peer_closed_session(
      config, peer_closed_socket, arena, metrics);
  peer_closed_session.SetConsumer(consumer);
  peer_closed_session.DriveRead();
  EXPECT_TRUE(peer_closed_session.ShouldReconnect());
  EXPECT_EQ(peer_closed_session.LastError(), ConnectionError::kPeerClosed);

  ConnectionConfig heartbeat_config = config;
  heartbeat_config.prepared_write_slots = 1;
  PreparedWriteArena heartbeat_arena(heartbeat_config.prepared_write_slots,
                                     heartbeat_config.prepared_write_bytes);
  Metrics heartbeat_metrics{};
  FakeTlsSocket heartbeat_socket;
  CriticalSession<FakeTlsSocket> heartbeat_session(
      heartbeat_config, heartbeat_socket, heartbeat_arena, heartbeat_metrics);
  heartbeat_session.SetConsumer(consumer);
  heartbeat_session.AdvanceHeartbeat(2'000'000'000ULL);
  heartbeat_session.AdvanceHeartbeat(40'000'000'000ULL);
  EXPECT_TRUE(heartbeat_session.ShouldReconnect());
  EXPECT_EQ(heartbeat_session.LastError(), ConnectionError::kHeartbeatTimeout);
  EXPECT_EQ(heartbeat_metrics.heartbeat_timeouts, 1U);
}

namespace {

struct BackpressureCounter {
  size_t calls{0};
};

DeliveryResult CountAndBackpressure(void* context,
                                    const MessageView&) noexcept {
  ++static_cast<BackpressureCounter*>(context)->calls;
  return DeliveryResult::kBackpressured;
}

DeliveryResult FatalDelivery(void*, const MessageView&) noexcept {
  return DeliveryResult::kFatal;
}

ConnectionConfig BuildSmallConfig(size_t slots) {
  ConnectionConfig config{};
  config.prepared_write_slots = slots;
  config.prepared_write_bytes = 128;
  config.read_buffer_bytes = 256;
  config.frame_buffer_bytes = 256;
  return config;
}

}  // namespace

TEST(WebsocketCriticalSessionTest,
     BackpressuredConsumerDropsFramesWithoutReconnect) {
  auto config = BuildSmallConfig(4);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  BackpressureCounter counter{};
  MessageConsumer consumer{&counter, &CountAndBackpressure};
  FakeTlsSocket socket;

  auto first = BuildServerTextFrame("aa");
  auto second = BuildServerTextFrame("bb");
  std::vector<std::byte> coalesced;
  coalesced.insert(coalesced.end(), first.begin(), first.end());
  coalesced.insert(coalesced.end(), second.begin(), second.end());
  socket.read_chunks_.push_back(coalesced);

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);
  session.DriveRead();

  EXPECT_FALSE(session.ShouldReconnect());
  EXPECT_EQ(session.LastError(), ConnectionError::kNone);
  EXPECT_EQ(counter.calls, 2U);
  EXPECT_EQ(metrics.consumer_backpressure_drops, 2U);
  EXPECT_EQ(metrics.rx_messages, 0U);
}

TEST(WebsocketCriticalSessionTest,
     FatalConsumerTriggersReconnectWithConsumerFatal) {
  auto config = BuildSmallConfig(4);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  MessageConsumer consumer{nullptr, &FatalDelivery};
  FakeTlsSocket socket;
  socket.read_chunks_.push_back(BuildServerTextFrame("x"));

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);
  session.DriveRead();

  EXPECT_TRUE(session.ShouldReconnect());
  EXPECT_EQ(session.LastError(), ConnectionError::kConsumerFatal);
  EXPECT_EQ(metrics.consumer_backpressure_drops, 0U);
}

TEST(WebsocketCriticalSessionTest,
     AutoPongEnqueueFailureSkipsWithoutReconnect) {
  auto config = BuildSmallConfig(1);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.read_chunks_.push_back(BuildServerPingFrame("z"));

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);

  // Exhaust the single slot with a business commit so auto-pong has no room.
  auto* hold = session.TryAcquirePreparedWrite();
  ASSERT_NE(hold, nullptr);
  hold->encoded_size = 4;
  hold->kind = PayloadKind::kText;
  ASSERT_EQ(session.CommitPreparedWrite(hold), SendStatus::kOk);

  session.DriveRead();

  EXPECT_FALSE(session.ShouldReconnect());
  EXPECT_EQ(session.LastError(), ConnectionError::kNone);
  EXPECT_EQ(metrics.control_frame_enqueue_failures, 1U);
}

TEST(WebsocketCriticalSessionTest,
     HeartbeatPingEnqueueFailureSkipsTickWithoutReconnect) {
  auto config = BuildSmallConfig(1);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);

  // Exhaust the single slot so the heartbeat ping cannot be queued.
  auto* hold = session.TryAcquirePreparedWrite();
  ASSERT_NE(hold, nullptr);
  hold->encoded_size = 4;
  hold->kind = PayloadKind::kText;
  ASSERT_EQ(session.CommitPreparedWrite(hold), SendStatus::kOk);

  session.AdvanceHeartbeat(6'000'000'000ULL);

  EXPECT_FALSE(session.ShouldReconnect());
  EXPECT_EQ(session.LastError(), ConnectionError::kNone);
  EXPECT_EQ(metrics.control_frame_enqueue_failures, 1U);
  EXPECT_EQ(metrics.heartbeat_timeouts, 0U);
}

TEST(WebsocketCriticalSessionTest,
     FrameCodecCapacityExhaustionIsObservableWithoutReconnect) {
  auto config = BuildSmallConfig(4);
  config.ready_frame_slots = 0;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.read_chunks_.push_back(BuildServerTextFrame("x"));

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);
  session.DriveRead();

  EXPECT_FALSE(session.ShouldReconnect());
  EXPECT_EQ(session.LastError(), ConnectionError::kNone);
  EXPECT_EQ(metrics.frame_codec_capacity_exhaustions, 1U);
  EXPECT_EQ(metrics.rx_messages, 0U);
  EXPECT_EQ(bytes, 0U);
}

TEST(WebsocketCriticalSessionTest, ResetClearsPendingAndFlags) {
  auto config = BuildSmallConfig(2);
  config.heartbeat_interval_ms = 1;
  config.heartbeat_timeout_ms = 5;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);

  session.AdvanceHeartbeat(1'000'000ULL);
  EXPECT_TRUE(session.WantsWrite());
  socket.read_chunks_.push_back({std::byte{0x81}});
  session.DriveRead();
  session.AdvanceHeartbeat(20'000'000ULL);
  ASSERT_TRUE(session.ShouldReconnect());
  ASSERT_EQ(session.LastError(), ConnectionError::kHeartbeatTimeout);

  session.Reset();

  EXPECT_FALSE(session.ShouldReconnect());
  EXPECT_EQ(session.LastError(), ConnectionError::kNone);
  EXPECT_FALSE(session.WantsWrite());

  std::vector<PreparedWrite*> acquired;
  for (size_t i = 0; i < config.prepared_write_slots; ++i) {
    auto* write = arena.TryAcquire();
    ASSERT_NE(write, nullptr);
    acquired.push_back(write);
  }
  EXPECT_EQ(arena.TryAcquire(), nullptr);
  for (auto* write : acquired) {
    arena.Release(write);
  }

  socket.read_chunks_.push_back(BuildServerTextFrame("ok"));
  session.DriveRead();
  EXPECT_EQ(bytes, 2U);
  EXPECT_EQ(metrics.rx_messages, 1U);
  EXPECT_FALSE(session.ShouldReconnect());

  session.AdvanceHeartbeat(21'000'000ULL);
  EXPECT_TRUE(session.WantsWrite());
  EXPECT_EQ(session.LastError(), ConnectionError::kNone);
}
