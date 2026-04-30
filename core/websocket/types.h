#ifndef AQUILA_CORE_WEBSOCKET_TYPES_H_
#define AQUILA_CORE_WEBSOCKET_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/websocket/runtime_policy.h"

namespace aquila::websocket {

enum class ConnectionPhase : std::uint8_t {
  kDisconnected,
  kResolving,
  kTcpConnecting,
  kTlsHandshaking,
  kWsHandshaking,
  kActive,
  kDegraded,
  kReconnectBackoff,
  kClosing,
  kClosed,
};

enum class ConnectionError : std::uint8_t {
  kNone,
  kResolveFailure,
  kSocketError,
  kConnectTimeout,
  kTlsFailure,
  kHandshakeFailure,
  kProtocolError,
  kHeartbeatTimeout,
  kPeerClosed,
  kConsumerFatal,
};

enum class PayloadKind : std::uint8_t {
  kText,
  kBinary,
  kPing,
  kPong,
  kClose,
};

enum class DeliveryResult : std::uint8_t {
  kAccepted,
  kBackpressured,
  kFatal,
};

enum class SendStatus : std::uint8_t {
  kOk,
  kNoPreparedWriteSlot,
  kWriteUnavailable,
  kEncodeFailed,
  kPayloadTooLarge,
};

enum class WriteFlushMode : std::uint8_t {
  kQueued,
  // After enqueue, try at most one business WriteSome without draining the
  // queue. EAGAIN and partial writes remain pending for the normal write pump.
  kTryFlushOne,
};

struct ReconnectPolicy {
  bool enabled = true;
  std::uint32_t initial_backoff_ms = 100;
  std::uint32_t max_backoff_ms = 30'000;
  // Multiplier = 1 << shift_bits. 0 keeps the backoff constant.
  std::uint8_t backoff_shift_bits = 1;
  // Integer jitter in [-jitter_percent, +jitter_percent].
  std::uint8_t jitter_percent = 25;
  // 0 means retry until Stop() or a fatal-class error.
  std::uint32_t max_attempts = 0;
};

struct DegradedThresholds {
  // High watermark expressed as percent to avoid floating point on the path.
  std::uint8_t high_watermark_percent = 80;
  std::uint32_t high_watermark_hold_ticks = 8;
  std::uint32_t recover_ticks = 16;
  std::uint32_t backpressure_drops_per_second = 10;
  std::uint32_t awaiting_pong_timeout_ms = 3000;
  // Capacity exhaustion events per second that move the session to degraded.
  // Keep this strict in production: capacity exhaustion means bounded receive
  // storage could not make progress, not a normal market-data burst.
  std::uint32_t frame_codec_capacity_events_per_second = 1;
  // 0 reuses RuntimePolicy::spin_iterations_before_clock_check.
  std::uint32_t evaluation_interval_iterations = 0;
};

struct DefaultWebSocketOptions {
  static constexpr ClockSource kClockSource = ClockSource::kSteady;
  static constexpr size_t kPreparedWriteSlots = 2048;
  static constexpr size_t kPreparedWriteBytes = 4096;
};

struct ConnectionConfig {
  std::string host = "fx-ws.gateio.ws";
  std::string service = "443";
  std::string target = "/v4/ws/usdt";
  bool enable_tls = true;
  // Reserved for legacy/external-buffer read paths. CriticalSession's hot path
  // reads directly into FrameCodec's mirrored receive ring instead.
  size_t read_buffer_bytes = size_t{1} << 20;
  // Requested mirrored receive-ring capacity. FrameCodec raises this to at
  // least max_frame_payload_bytes plus the maximum WebSocket frame header, and
  // MirroredBuffer rounds the actual capacity to a page-aligned power of two.
  size_t frame_buffer_bytes = size_t{1} << 20;
  // Maximum accepted single-frame payload. Keep this close to the largest legal
  // exchange message; larger payloads are protocol errors, not capacity events.
  size_t max_frame_payload_bytes = size_t{1} << 20;
  // Maximum ReadSome calls per DriveRead. 1 preserves the legacy single-read
  // path; larger values only help when the read pump is allowed to continue.
  std::uint32_t max_reads_per_drive = 1;
  // When false, extra reads only happen when the socket reports buffered
  // plaintext. When true, DriveRead may continue until EAGAIN or the read
  // budget is exhausted.
  bool read_until_would_block = false;
  size_t prepared_write_slots = DefaultWebSocketOptions::kPreparedWriteSlots;
  size_t prepared_write_bytes = DefaultWebSocketOptions::kPreparedWriteBytes;
  // Maximum complete business frames written per DriveWrite after partial
  // writes and control frames are handled. 0 keeps the legacy drain-until-empty
  // behavior; 1 favors read-path tail latency under order bursts.
  std::uint32_t max_business_writes_per_drive = 1;
  std::uint32_t heartbeat_interval_ms = 5000;
  std::uint32_t heartbeat_timeout_ms = 15000;
  // Total wall-clock budget for the cold path (DNS + TCP + TLS + WS handshake).
  // Synchronous getaddrinfo is not interruptible and is counted into this.
  std::uint32_t cold_path_total_timeout_ms = 10000;
  ReconnectPolicy reconnect{};
  DegradedThresholds degraded{};
  RuntimePolicy runtime_policy{};
};

template <typename OptionsT = DefaultWebSocketOptions>
[[nodiscard]] inline ConnectionConfig MakeConnectionConfig() {
  ConnectionConfig config{};
  config.runtime_policy.clock_source = OptionsT::kClockSource;
  config.prepared_write_slots = OptionsT::kPreparedWriteSlots;
  config.prepared_write_bytes = OptionsT::kPreparedWriteBytes;
  return config;
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_TYPES_H_
