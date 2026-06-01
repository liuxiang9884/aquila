#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_TYPES_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/common/types.h"
#include "core/websocket/socket_diagnostics.h"
#include "core/websocket/socket_timestamping.h"
#include "exchange/gate/trading/order_latency_diagnostics.h"

namespace aquila::gate {

inline constexpr std::size_t kDefaultOrderRequestMapCapacity = 16384;

enum class OrderRequestType : std::uint8_t {
  kUnknown = 0,
  kLogin = 1,
  kPlaceOrder = 2,
  kCancelOrder = 3,
};

struct DecodedRequestId {
  bool ok{false};
  OrderRequestType type{OrderRequestType::kUnknown};
  std::uint64_t sequence{0};
};

struct ParsedOrderText {
  bool ok{false};
  std::uint64_t local_order_id{0};
};

enum class OrderSendStatus : std::uint8_t {
  kOk,
  kNotLoggedIn,
  kNotActive,
  kInflightFull,
  kEncodeBufferTooSmall,
  kInvalidLocalOrderId,
  kUnsupportedOrderType,
  kInvalidQuantityText,
  kSignatureFailed,
  kNoPreparedWriteSlot,
  kWriteUnavailable,
};

struct OrderSendResult {
  OrderSendStatus status{OrderSendStatus::kNotActive};
  std::uint64_t request_sequence{0};
  std::uint64_t encoded_request_id{0};
  std::int64_t send_local_ns{0};
};

struct OrderSessionConnectionInfo {
  std::uint64_t order_session_id{0};
  int owner_thread_cpu{-1};
  int owner_thread_tid{-1};
  bool endpoint_available{false};
  std::string local_ip;
  std::uint16_t local_port{0};
  std::string remote_ip;
  std::uint16_t remote_port{0};
};

enum class OrderResponseKind : std::uint8_t {
  kAck,
  kAccepted,
  kRejected,
  kCancelAccepted,
  kCancelRejected,
};

struct OrderResponse {
  OrderResponseKind kind{OrderResponseKind::kAck};
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t request_sequence{0};
  std::uint16_t http_status{0};
  std::uint64_t error_label_hash{0};
  std::int64_t local_receive_ns{0};
  std::int64_t exchange_ns{0};
  std::int64_t exchange_x_in_ns{0};
  std::int64_t exchange_x_out_ns{0};
  std::int64_t exchange_x_in_to_x_out_ns{0};
  websocket::SocketTimestampingSnapshot socket_timestamps{};
  websocket::SocketTimestampingStages socket_timestamp_stages{};
  bool ack_latency_diagnostic_available{false};
  OrderLatencyDiagnosticLogRecord ack_latency_diagnostic{};
  bool tcp_info_requested{false};
  websocket::TcpInfoDiagnostics tcp_info{};
};

struct OrderSessionStats {
  std::uint64_t text_messages{0};
  std::uint64_t parse_errors{0};
  std::uint64_t ignored_messages{0};
  std::uint64_t login_sent{0};
  std::uint64_t login_accepted{0};
  std::uint64_t login_rejected{0};
  std::uint64_t place_sent{0};
  std::uint64_t cancel_sent{0};
  std::uint64_t responses{0};
  std::uint64_t unknown_request_ids{0};
  std::uint64_t local_send_failures{0};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_TYPES_H_
