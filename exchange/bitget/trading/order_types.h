#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_TYPES_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/common/types.h"

namespace aquila::bitget {

inline constexpr std::size_t kDefaultOrderRequestMapCapacity = 16384;
inline constexpr std::size_t kDefaultOrderIdCacheCapacity = 16384;

enum class OrderRequestType : std::uint8_t {
  kUnknown = 0,
  kPlaceOrder = 1,
  kCancelOrder = 2,
};

struct DecodedRequestId {
  bool ok{false};
  OrderRequestType type{OrderRequestType::kUnknown};
  std::uint64_t sequence{0};
};

struct ParsedClientOid {
  bool ok{false};
  std::uint64_t local_order_id{0};
};

enum class OrderSendStatus : std::uint8_t {
  kOk,
  kNotLoggedIn,
  kNotActive,
  kInflightFull,
  kOrderIdCacheFull,
  kUnsupportedOrderType,
  kInvalidSymbol,
  kInvalidQuantityText,
  kInvalidPriceText,
  kInvalidLocalOrderId,
  kSignatureFailed,
  kEncodeBufferTooSmall,
  kNoPreparedWriteSlot,
  kWriteUnavailable,
};

struct OrderSendResult {
  OrderSendStatus status{OrderSendStatus::kNotActive};
  std::uint64_t request_sequence{0};
  std::uint64_t encoded_request_id{0};
  std::int64_t send_local_ns{0};
};

enum class OrderResponseKind : std::uint8_t {
  kAck,
  kRejected,
  kCancelRejected,
  kUnknownResult,
};

struct OrderResponse {
  OrderResponseKind kind{OrderResponseKind::kAck};
  OrderRequestType request_type{OrderRequestType::kUnknown};
  std::uint64_t local_order_id{0};
  std::uint64_t parent_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t request_sequence{0};
  std::uint16_t route_id{static_cast<std::uint16_t>(0xFFFF)};
  std::uint32_t error_code{0};
  std::uint64_t connection_id_hash{0};
  std::int64_t request_send_local_ns{0};
  std::int64_t local_receive_ns{0};
  std::int64_t exchange_ns{0};
  std::int64_t ack_rtt_ns{-1};
};

struct LoginCredentials {
  std::string api_key;
  std::string api_secret;
  std::string passphrase;
};

struct OrderSessionStats {
  std::uint64_t text_messages{0};
  std::uint64_t parse_errors{0};
  std::uint64_t ignored_messages{0};
  std::uint64_t login_sent{0};
  std::uint64_t login_accepted{0};
  std::uint64_t login_rejected{0};
  std::uint64_t pings_sent{0};
  std::uint64_t pongs_received{0};
  std::uint64_t heartbeat_timeouts{0};
  std::uint64_t place_sent{0};
  std::uint64_t cancel_sent{0};
  std::uint64_t responses{0};
  std::uint64_t unknown_request_ids{0};
  std::uint64_t correlation_mismatches{0};
  std::uint64_t local_send_failures{0};
};

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_TYPES_H_
