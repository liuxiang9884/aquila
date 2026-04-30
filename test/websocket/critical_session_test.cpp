#include "core/websocket/critical_session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/websocket/cold_path_loop.h"

using namespace aquila::websocket;

namespace {

DeliveryResult RecordMessage(void* context, const MessageView& view) noexcept {
  auto* bytes = static_cast<size_t*>(context);
  *bytes += view.payload.size();
  return DeliveryResult::kAccepted;
}

struct TypedByteCounter {
  size_t bytes{0};

  DeliveryResult Handle(const MessageView& view) noexcept {
    bytes += view.payload.size();
    return DeliveryResult::kAccepted;
  }
};

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

std::vector<std::byte> DecodeMaskedClientPayload(
    std::span<const std::byte> frame) {
  if (frame.size() < 6) {
    return {};
  }
  const auto encoded_length = std::to_integer<unsigned char>(frame[1]);
  const size_t payload_size = encoded_length & 0x7FU;
  if ((encoded_length & 0x80U) == 0 || frame.size() != 6 + payload_size) {
    return {};
  }

  const std::array<std::byte, 4> mask_key{frame[2], frame[3], frame[4],
                                          frame[5]};
  std::vector<std::byte> payload(payload_size);
  for (size_t i = 0; i < payload_size; ++i) {
    payload[i] = frame[6 + i] ^ mask_key[i & 0x3U];
  }
  return payload;
}

class FakeTlsSocket final {
 public:
  ssize_t ReadSome(std::span<std::byte> buffer) noexcept {
    ++read_calls_;
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
      chunk.erase(chunk.begin(),
                  chunk.begin() + static_cast<std::ptrdiff_t>(bytes));
    }
    return static_cast<ssize_t>(bytes);
  }

  size_t PendingReadableBytes() const noexcept {
    return pending_readable_ && !read_chunks_.empty()
               ? read_chunks_.front().size()
               : 0;
  }

  ssize_t WriteSome(std::span<const std::byte> buffer) noexcept {
    if (pending_write_eagain_) {
      pending_write_eagain_ = false;
      errno = EAGAIN;
      return -1;
    }

    const size_t chunk =
        max_write_bytes_per_call_ == 0
            ? buffer.size()
            : std::min(buffer.size(), max_write_bytes_per_call_);
    written_.insert(written_.end(), buffer.begin(), buffer.begin() + chunk);
    if (chunk < buffer.size() && eagain_after_partial_write_) {
      pending_write_eagain_ = true;
    }
    return static_cast<ssize_t>(chunk);
  }

  size_t read_calls_{0};
  bool pending_readable_{false};
  size_t max_write_bytes_per_call_{0};
  bool eagain_after_partial_write_{true};
  bool pending_write_eagain_{false};
  bool eof_on_empty_{false};
  std::vector<std::vector<std::byte>> read_chunks_{};
  std::vector<std::byte> written_{};
};

struct CallbackWriteContext {
  CriticalSession<FakeTlsSocket>* session{nullptr};
  std::span<const std::byte> payload{};
  WriteFlushMode flush_mode{WriteFlushMode::kQueued};
  SendStatus status{SendStatus::kWriteUnavailable};
};

DeliveryResult CommitWriteFromCallback(void* context,
                                       const MessageView&) noexcept {
  auto* state = static_cast<CallbackWriteContext*>(context);
  PreparedWrite* write = state->session->TryAcquirePreparedWrite();
  if (write == nullptr) {
    state->status = SendStatus::kNoPreparedWriteSlot;
    return DeliveryResult::kAccepted;
  }

  std::copy(state->payload.begin(), state->payload.end(),
            write->storage.begin());
  write->encoded_size = static_cast<std::uint32_t>(state->payload.size());
  write->kind = PayloadKind::kBinary;
  state->status = state->session->CommitPreparedWrite(write, state->flush_mode);
  if (state->status != SendStatus::kOk) {
    state->session->CancelPreparedWrite(write);
  }
  return DeliveryResult::kAccepted;
}

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
  MessageCallback consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.max_write_bytes_per_call_ = 2;

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);

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
  CriticalSession<FakeTlsSocket> peer_closed_session(config, peer_closed_socket,
                                                     arena, metrics);
  peer_closed_session.SetMessageCallback(consumer);
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
  heartbeat_session.SetMessageCallback(consumer);
  heartbeat_session.AdvanceHeartbeat(2'000'000'000ULL);
  heartbeat_session.AdvanceHeartbeat(40'000'000'000ULL);
  EXPECT_TRUE(heartbeat_session.ShouldReconnect());
  EXPECT_EQ(heartbeat_session.LastError(), ConnectionError::kHeartbeatTimeout);
  EXPECT_EQ(heartbeat_metrics.heartbeat_timeouts, 1U);
}

TEST(WebsocketCriticalSessionTest, SupportsTypedMessageHandler) {
  ConnectionConfig config{};
  config.prepared_write_slots = 4;
  config.prepared_write_bytes = 128;
  config.read_buffer_bytes = 256;
  config.frame_buffer_bytes = 256;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  TypedByteCounter counter;
  auto handler = MakeMessageHandler(counter);
  FakeTlsSocket socket;
  socket.read_chunks_.push_back(BuildServerTextFrame("md"));

  CriticalSession<FakeTlsSocket, decltype(handler)> session(config, socket,
                                                            arena, metrics);
  session.SetMessageHandler(handler);
  session.DriveRead();

  EXPECT_FALSE(session.ShouldReconnect());
  EXPECT_EQ(counter.bytes, 2U);
  EXPECT_EQ(metrics.rx_messages, 1U);
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
  MessageCallback consumer{&counter, &CountAndBackpressure};
  FakeTlsSocket socket;

  auto first = BuildServerTextFrame("aa");
  auto second = BuildServerTextFrame("bb");
  std::vector<std::byte> coalesced;
  coalesced.insert(coalesced.end(), first.begin(), first.end());
  coalesced.insert(coalesced.end(), second.begin(), second.end());
  socket.read_chunks_.push_back(coalesced);

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);
  session.DriveRead();

  EXPECT_FALSE(session.ShouldReconnect());
  EXPECT_EQ(session.LastError(), ConnectionError::kNone);
  EXPECT_EQ(counter.calls, 2U);
  EXPECT_EQ(metrics.consumer_backpressure_drops, 2U);
  EXPECT_EQ(metrics.rx_messages, 0U);
}

TEST(WebsocketCriticalSessionTest,
     CommitPreparedWriteTryFlushOneWritesImmediately) {
  auto config = BuildSmallConfig(2);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  FakeTlsSocket socket;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);

  auto* write = session.TryAcquirePreparedWrite();
  ASSERT_NE(write, nullptr);
  std::copy_n(std::as_bytes(std::span{"abcd", 4}).begin(), 4,
              write->storage.begin());
  write->encoded_size = 4;
  write->kind = PayloadKind::kBinary;

  EXPECT_EQ(session.CommitPreparedWrite(write, WriteFlushMode::kTryFlushOne),
            SendStatus::kOk);

  ASSERT_EQ(socket.written_.size(), 4U);
  EXPECT_EQ(static_cast<char>(socket.written_[0]), 'a');
  EXPECT_EQ(static_cast<char>(socket.written_[3]), 'd');
  EXPECT_EQ(metrics.tx_messages, 1U);
  EXPECT_EQ(metrics.tx_bytes, 4U);
  EXPECT_EQ(session.PendingWriteCount(), 0U);
  EXPECT_FALSE(session.WantsWrite());
}

TEST(WebsocketCriticalSessionTest,
     CommitPreparedWriteTryFlushOneKeepsPendingOnEagain) {
  auto config = BuildSmallConfig(2);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  FakeTlsSocket socket;
  socket.pending_write_eagain_ = true;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);

  auto* write = session.TryAcquirePreparedWrite();
  ASSERT_NE(write, nullptr);
  std::copy_n(std::as_bytes(std::span{"abcd", 4}).begin(), 4,
              write->storage.begin());
  write->encoded_size = 4;
  write->kind = PayloadKind::kBinary;

  EXPECT_EQ(session.CommitPreparedWrite(write, WriteFlushMode::kTryFlushOne),
            SendStatus::kOk);
  EXPECT_TRUE(socket.written_.empty());
  EXPECT_EQ(metrics.tx_messages, 0U);
  EXPECT_EQ(session.PendingWriteCount(), 1U);
  EXPECT_TRUE(session.WantsWrite());

  session.DriveWrite();

  ASSERT_EQ(socket.written_.size(), 4U);
  EXPECT_EQ(metrics.tx_messages, 1U);
  EXPECT_EQ(session.PendingWriteCount(), 0U);
  EXPECT_FALSE(session.WantsWrite());
}

TEST(WebsocketCriticalSessionTest,
     CommitPreparedWriteTryFlushOnePreservesQueuedOrder) {
  auto config = BuildSmallConfig(2);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  FakeTlsSocket socket;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);

  auto* first = session.TryAcquirePreparedWrite();
  ASSERT_NE(first, nullptr);
  std::copy_n(std::as_bytes(std::span{"old1", 4}).begin(), 4,
              first->storage.begin());
  first->encoded_size = 4;
  first->kind = PayloadKind::kBinary;
  ASSERT_EQ(session.CommitPreparedWrite(first), SendStatus::kOk);

  auto* second = session.TryAcquirePreparedWrite();
  ASSERT_NE(second, nullptr);
  std::copy_n(std::as_bytes(std::span{"new2", 4}).begin(), 4,
              second->storage.begin());
  second->encoded_size = 4;
  second->kind = PayloadKind::kBinary;

  EXPECT_EQ(session.CommitPreparedWrite(second, WriteFlushMode::kTryFlushOne),
            SendStatus::kOk);

  ASSERT_EQ(socket.written_.size(), 4U);
  EXPECT_EQ(static_cast<char>(socket.written_[0]), 'o');
  EXPECT_EQ(static_cast<char>(socket.written_[3]), '1');
  EXPECT_EQ(session.PendingWriteCount(), 1U);

  session.DriveWrite();

  ASSERT_EQ(socket.written_.size(), 8U);
  EXPECT_EQ(static_cast<char>(socket.written_[4]), 'n');
  EXPECT_EQ(static_cast<char>(socket.written_[7]), '2');
  EXPECT_EQ(metrics.tx_messages, 2U);
}

TEST(WebsocketCriticalSessionTest,
     CommitPreparedWriteTryFlushOneDoesNotBypassPendingControlFrame) {
  auto config = BuildSmallConfig(2);
  config.heartbeat_interval_ms = 1;
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  FakeTlsSocket socket;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);

  session.AdvanceHeartbeat(2'000'000ULL);
  ASSERT_TRUE(session.WantsWrite());

  auto* business = session.TryAcquirePreparedWrite();
  ASSERT_NE(business, nullptr);
  std::copy_n(std::as_bytes(std::span{"abcd", 4}).begin(), 4,
              business->storage.begin());
  business->encoded_size = 4;
  business->kind = PayloadKind::kBinary;
  EXPECT_EQ(session.CommitPreparedWrite(business, WriteFlushMode::kTryFlushOne),
            SendStatus::kOk);

  EXPECT_TRUE(socket.written_.empty());
  EXPECT_EQ(session.PendingWriteCount(), 1U);

  session.DriveWrite();

  ASSERT_GE(socket.written_.size(), 10U);
  EXPECT_EQ(socket.written_[0], std::byte{0x89});
  EXPECT_EQ(static_cast<char>(socket.written_[6]), 'a');
  EXPECT_EQ(static_cast<char>(socket.written_[9]), 'd');
  EXPECT_EQ(metrics.tx_messages, 2U);
  EXPECT_EQ(session.PendingWriteCount(), 0U);
}

TEST(WebsocketCriticalSessionTest, SendTextEncodesMaskedFrameThroughCoreCodec) {
  auto config = BuildSmallConfig(2);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  FakeTlsSocket socket;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);

  const auto payload = std::as_bytes(std::span{"tick", 4});
  EXPECT_EQ(session.SendText(payload), SendStatus::kOk);
  EXPECT_EQ(session.PendingWriteCount(), 1U);

  session.DriveWrite();

  ASSERT_EQ(socket.written_.size(), 10U);
  EXPECT_EQ(socket.written_[0], std::byte{0x81});
  EXPECT_EQ(socket.written_[1], std::byte{0x84});
  const auto decoded = DecodeMaskedClientPayload(std::span<const std::byte>(
      socket.written_.data(), socket.written_.size()));
  ASSERT_EQ(decoded.size(), payload.size());
  EXPECT_TRUE(std::equal(decoded.begin(), decoded.end(), payload.begin()));
  EXPECT_EQ(metrics.tx_messages, 1U);
  EXPECT_EQ(session.PendingWriteCount(), 0U);
}

TEST(WebsocketCriticalSessionTest, SendTextReleasesSlotAfterEncodeFailure) {
  auto config = BuildSmallConfig(1);
  config.prepared_write_bytes = 6;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  FakeTlsSocket socket;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);

  EXPECT_EQ(session.SendText(std::as_bytes(std::span{"x", 1})),
            SendStatus::kEncodeFailed);
  EXPECT_EQ(session.PendingWriteCount(), 0U);
  EXPECT_EQ(session.SendText(std::span<const std::byte>{}), SendStatus::kOk);
}

TEST(WebsocketCriticalSessionTest, ReadCallbackCanFlushWriteImmediately) {
  auto config = BuildSmallConfig(2);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  FakeTlsSocket socket;
  socket.read_chunks_.push_back(BuildServerTextFrame("md"));
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  const std::array<std::byte, 4> payload{std::byte{'o'}, std::byte{'r'},
                                         std::byte{'d'}, std::byte{'r'}};
  CallbackWriteContext context{&session, payload, WriteFlushMode::kTryFlushOne};
  session.SetMessageCallback(
      MessageCallback{&context, &CommitWriteFromCallback});

  session.DriveRead();

  EXPECT_EQ(context.status, SendStatus::kOk);
  ASSERT_EQ(socket.written_.size(), payload.size());
  EXPECT_EQ(static_cast<char>(socket.written_[0]), 'o');
  EXPECT_EQ(static_cast<char>(socket.written_[3]), 'r');
  EXPECT_EQ(metrics.rx_messages, 1U);
  EXPECT_EQ(metrics.tx_messages, 1U);
  EXPECT_EQ(session.PendingWriteCount(), 0U);
  EXPECT_FALSE(session.WantsWrite());
}

TEST(WebsocketCriticalSessionTest, BoundedReadPumpDrainsPendingChunks) {
  auto config = BuildSmallConfig(4);
  config.max_reads_per_drive = 3;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageCallback consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.pending_readable_ = true;
  socket.read_chunks_.push_back(BuildServerTextFrame("a"));
  socket.read_chunks_.push_back(BuildServerTextFrame("b"));
  socket.read_chunks_.push_back(BuildServerTextFrame("c"));

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);
  session.DriveRead();

  EXPECT_EQ(bytes, 3U);
  EXPECT_EQ(metrics.rx_messages, 3U);
  EXPECT_EQ(metrics.rx_bytes, 9U);
  EXPECT_EQ(socket.read_calls_, 3U);
}

TEST(WebsocketCriticalSessionTest, BoundedReadPumpDoesNotProbeWithoutPending) {
  auto config = BuildSmallConfig(4);
  config.max_reads_per_drive = 3;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageCallback consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.pending_readable_ = false;
  socket.read_chunks_.push_back(BuildServerTextFrame("a"));
  socket.read_chunks_.push_back(BuildServerTextFrame("b"));

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);
  session.DriveRead();

  EXPECT_EQ(bytes, 1U);
  EXPECT_EQ(metrics.rx_messages, 1U);
  EXPECT_EQ(metrics.rx_bytes, 3U);
  EXPECT_EQ(socket.read_calls_, 1U);
}

TEST(WebsocketCriticalSessionTest,
     FatalConsumerTriggersReconnectWithConsumerFatal) {
  auto config = BuildSmallConfig(4);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  MessageCallback consumer{nullptr, &FatalDelivery};
  FakeTlsSocket socket;
  socket.read_chunks_.push_back(BuildServerTextFrame("x"));

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);
  session.DriveRead();

  EXPECT_TRUE(session.ShouldReconnect());
  EXPECT_EQ(session.LastError(), ConnectionError::kConsumerFatal);
  EXPECT_EQ(metrics.consumer_backpressure_drops, 0U);
}

TEST(WebsocketCriticalSessionTest, AutoPongUsesControlSlotWhenQueueFull) {
  auto config = BuildSmallConfig(1);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageCallback consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.read_chunks_.push_back(BuildServerPingFrame("z"));

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);

  // Exhaust the single slot with a business commit so auto-pong has no room.
  auto* hold = session.TryAcquirePreparedWrite();
  ASSERT_NE(hold, nullptr);
  hold->encoded_size = 4;
  hold->kind = PayloadKind::kText;
  ASSERT_EQ(session.CommitPreparedWrite(hold), SendStatus::kOk);

  session.DriveRead();

  EXPECT_FALSE(session.ShouldReconnect());
  EXPECT_EQ(session.LastError(), ConnectionError::kNone);
  EXPECT_EQ(metrics.control_frame_enqueue_failures, 0U);

  session.DriveWrite();
  EXPECT_FALSE(socket.written_.empty());
}

TEST(WebsocketCriticalSessionTest, HeartbeatPingUsesControlSlotWhenQueueFull) {
  auto config = BuildSmallConfig(1);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageCallback consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);

  // Exhaust the single slot so the heartbeat ping cannot be queued.
  auto* hold = session.TryAcquirePreparedWrite();
  ASSERT_NE(hold, nullptr);
  hold->encoded_size = 4;
  hold->kind = PayloadKind::kText;
  ASSERT_EQ(session.CommitPreparedWrite(hold), SendStatus::kOk);

  session.AdvanceHeartbeat(6'000'000'000ULL);

  EXPECT_FALSE(session.ShouldReconnect());
  EXPECT_EQ(session.LastError(), ConnectionError::kNone);
  EXPECT_EQ(metrics.control_frame_enqueue_failures, 0U);
  EXPECT_EQ(metrics.heartbeat_timeouts, 0U);

  session.DriveWrite();
  EXPECT_FALSE(socket.written_.empty());
}

TEST(WebsocketCriticalSessionTest,
     ControlWriteDoesNotInterruptPartialBusinessFrame) {
  auto config = BuildSmallConfig(2);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageCallback consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.max_write_bytes_per_call_ = 2;
  socket.eagain_after_partial_write_ = false;

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);

  auto* business = session.TryAcquirePreparedWrite();
  ASSERT_NE(business, nullptr);
  std::copy_n(std::as_bytes(std::span{"abcd", 4}).begin(), 4,
              business->storage.begin());
  business->encoded_size = 4;
  business->kind = PayloadKind::kText;
  ASSERT_EQ(session.CommitPreparedWrite(business), SendStatus::kOk);

  session.DriveWrite();
  ASSERT_EQ(socket.written_.size(), 4U);

  auto* second_business = session.TryAcquirePreparedWrite();
  ASSERT_NE(second_business, nullptr);
  std::copy_n(std::as_bytes(std::span{"wxyz", 4}).begin(), 4,
              second_business->storage.begin());
  second_business->encoded_size = 4;
  second_business->kind = PayloadKind::kText;
  ASSERT_EQ(session.CommitPreparedWrite(second_business), SendStatus::kOk);
  socket.max_write_bytes_per_call_ = 2;
  socket.eagain_after_partial_write_ = true;
  session.DriveWrite();
  ASSERT_EQ(socket.written_.size(), 6U);

  session.AdvanceHeartbeat(6'000'000'000ULL);
  session.DriveWrite();

  ASSERT_GE(socket.written_.size(), 8U);
  EXPECT_EQ(static_cast<char>(socket.written_[4]), 'w');
  EXPECT_EQ(static_cast<char>(socket.written_[5]), 'x');
  EXPECT_EQ(static_cast<char>(socket.written_[6]), 'y');
  EXPECT_EQ(static_cast<char>(socket.written_[7]), 'z');
}

TEST(WebsocketCriticalSessionTest, BusinessWriteBudgetLimitsCompletedFrames) {
  auto config = BuildSmallConfig(4);
  config.max_business_writes_per_drive = 1;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageCallback consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);

  for (size_t i = 0; i < 3; ++i) {
    auto* write = session.TryAcquirePreparedWrite();
    ASSERT_NE(write, nullptr);
    std::copy_n(std::as_bytes(std::span{"abcd", 4}).begin(), 4,
                write->storage.begin());
    write->encoded_size = 4;
    write->kind = PayloadKind::kText;
    ASSERT_EQ(session.CommitPreparedWrite(write), SendStatus::kOk);
  }

  session.DriveWrite();

  EXPECT_EQ(metrics.tx_messages, 1U);
  EXPECT_EQ(session.PendingWriteCount(), 2U);
  EXPECT_TRUE(session.WantsWrite());
}

TEST(WebsocketCriticalSessionTest, ZeroBusinessWriteBudgetDrainsQueue) {
  auto config = BuildSmallConfig(4);
  config.max_business_writes_per_drive = 0;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageCallback consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);

  for (size_t i = 0; i < 3; ++i) {
    auto* write = session.TryAcquirePreparedWrite();
    ASSERT_NE(write, nullptr);
    std::copy_n(std::as_bytes(std::span{"abcd", 4}).begin(), 4,
                write->storage.begin());
    write->encoded_size = 4;
    write->kind = PayloadKind::kText;
    ASSERT_EQ(session.CommitPreparedWrite(write), SendStatus::kOk);
  }

  session.DriveWrite();

  EXPECT_EQ(metrics.tx_messages, 3U);
  EXPECT_EQ(session.PendingWriteCount(), 0U);
  EXPECT_FALSE(session.WantsWrite());
}

TEST(WebsocketCriticalSessionTest,
     FrameCodecCapacityExhaustionIsObservableWithoutReconnect) {
  auto config = BuildSmallConfig(4);
  config.max_frame_payload_bytes = std::numeric_limits<size_t>::max();
  config.frame_buffer_bytes = std::numeric_limits<size_t>::max();
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageCallback consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);
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
  MessageCallback consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);

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
