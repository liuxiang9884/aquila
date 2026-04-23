#include "core/websocket/critical_session.h"
#include "core/websocket/cold_path_loop.h"

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
  std::vector<std::vector<std::byte>> read_chunks_{};
  std::vector<std::byte> written_{};
};

}  // namespace

int main() {
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
  if (write == nullptr) {
    return 1;
  }
  std::copy_n(std::as_bytes(std::span{"tick", 4}).begin(), 4,
              write->storage.begin());
  write->encoded_size = 4;
  write->kind = PayloadKind::kText;
  if (session.CommitPreparedWrite(write) != SendStatus::kOk) {
    return 1;
  }

  session.DriveWrite();
  if (socket.written_.size() != 2 || metrics.tx_messages != 0 ||
      metrics.tx_bytes != 2 || !session.WantsWrite()) {
    return 1;
  }

  session.DriveWrite();
  if (socket.written_.size() != 4 || metrics.tx_messages != 1 ||
      metrics.tx_bytes != 4 || session.WantsWrite()) {
    return 1;
  }

  auto* cancelled = session.TryAcquirePreparedWrite();
  if (cancelled == nullptr) {
    return 1;
  }
  session.CancelPreparedWrite(cancelled);
  auto* reacquired = session.TryAcquirePreparedWrite();
  if (reacquired == nullptr) {
    return 1;
  }
  session.CancelPreparedWrite(reacquired);

  auto first_frame = BuildServerTextFrame("ab");
  auto second_frame = BuildServerTextFrame("cd");
  std::vector<std::byte> coalesced;
  coalesced.reserve(first_frame.size() + second_frame.size());
  coalesced.insert(coalesced.end(), first_frame.begin(), first_frame.end());
  coalesced.insert(coalesced.end(), second_frame.begin(), second_frame.end());
  socket.read_chunks_.push_back(coalesced);
  session.DriveRead();
  if (bytes != 4 || metrics.rx_messages != 2 ||
      metrics.rx_bytes != coalesced.size()) {
    return 1;
  }

  const size_t written_before_ping = socket.written_.size();
  socket.max_write_bytes_per_call_ = 0;
  socket.read_chunks_.push_back(BuildServerPingFrame("z"));
  session.DriveRead();
  if (!session.WantsWrite()) {
    return 1;
  }
  session.DriveWrite();
  if (socket.written_.size() <= written_before_ping) {
    return 1;
  }

  size_t dropped_bytes = 0;
  MessageConsumer backpressured_consumer{&dropped_bytes, &BackpressureMessage};
  FakeTlsSocket backpressured_socket;
  backpressured_socket.read_chunks_.push_back(BuildServerTextFrame("bp"));
  CriticalSession<FakeTlsSocket> backpressured_session(
      config, backpressured_socket, arena, metrics);
  backpressured_session.SetConsumer(backpressured_consumer);
  backpressured_session.DriveRead();
  if (!backpressured_session.ShouldReconnect()) {
    return 1;
  }

  return 0;
}
