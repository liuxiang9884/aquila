#include "core/websocket/critical_session.h"
#include "core/websocket/cold_path_loop.h"

#include <gtest/gtest.h>

#include <algorithm>
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

DeliveryResult BackpressureMessage(void*, const MessageView&) noexcept {
  return DeliveryResult::kBackpressured;
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
    const auto chunk = std::move(read_chunks_.front());
    read_chunks_.erase(read_chunks_.begin());
    std::copy(chunk.begin(), chunk.end(), buffer.begin());
    return static_cast<ssize_t>(chunk.size());
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

  size_t dropped_bytes = 0;
  MessageConsumer backpressured_consumer{&dropped_bytes, &BackpressureMessage};
  FakeTlsSocket backpressured_socket;
  backpressured_socket.read_chunks_.push_back(BuildServerTextFrame("bp"));
  CriticalSession<FakeTlsSocket> backpressured_session(
      config, backpressured_socket, arena, metrics);
  backpressured_session.SetConsumer(backpressured_consumer);
  backpressured_session.DriveRead();
  EXPECT_TRUE(backpressured_session.ShouldReconnect());
  EXPECT_EQ(backpressured_session.LastError(), ConnectionError::kSocketError);

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
