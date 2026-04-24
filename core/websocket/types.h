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

struct ConnectionConfig {
  std::string host = "fx-ws.gateio.ws";
  std::string service = "443";
  std::string target = "/v4/ws/usdt";
  bool enable_tls = true;
  size_t read_buffer_bytes = size_t{1} << 20;
  size_t frame_buffer_bytes = size_t{1} << 20;
  size_t prepared_write_slots = 2048;
  size_t prepared_write_bytes = 4096;
  std::uint32_t heartbeat_interval_ms = 5000;
  std::uint32_t heartbeat_timeout_ms = 15000;
  RuntimePolicy runtime_policy{};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_TYPES_H_
