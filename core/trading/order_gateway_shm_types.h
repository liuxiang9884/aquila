#ifndef AQUILA_CORE_TRADING_ORDER_GATEWAY_SHM_TYPES_H_
#define AQUILA_CORE_TRADING_ORDER_GATEWAY_SHM_TYPES_H_

#include <cstddef>
#include <cstdint>

#include "core/common/types.h"
#include "core/trading/order_types.h"

namespace aquila::core {

inline constexpr std::uint32_t kOrderGatewayShmMagic = 0x41514F47U;
inline constexpr std::uint16_t kOrderGatewayShmVersion = 1;
inline constexpr std::size_t kMaxOrderGatewayRoutes = 16;
inline constexpr std::size_t kOrderGatewaySymbolBytes = 32;
inline constexpr std::size_t kOrderGatewayQuantityTextBytes = 32;
inline constexpr std::size_t kOrderGatewayPriceTextBytes = 32;

enum class OrderGatewayCommandKind : std::uint8_t {
  kNone = 0,
  kPlace = 1,
  kCancel = 2,
  kCacheExchangeOrderId = 3,
  kForgetExchangeOrderId = 4,
  kStop = 5,
};

enum class OrderGatewayEventKind : std::uint8_t {
  kNone = 0,
  kOrderResponse = 1,
  kCommandRejected = 2,
  kReady = 3,
  kNotReady = 4,
  kStopped = 5,
};

enum class OrderGatewayCommandRejectReason : std::uint8_t {
  kNone = 0,
  kInvalidCommand = 1,
  kSessionNotReady = 2,
  kSessionNotActive = 3,
  kInflightFull = 4,
  kEncodeFailed = 5,
  kNoPreparedWriteSlot = 6,
  kWriteUnavailable = 7,
  kUnsupportedOrderType = 8,
};

struct OrderGatewayQueueDescriptor {
  std::uint64_t offset{0};
  std::uint64_t bytes{0};
  std::uint32_t capacity{0};
  std::uint32_t slot_size{0};
};

struct OrderGatewayShmHeader {
  std::uint32_t magic{kOrderGatewayShmMagic};
  std::uint16_t version{kOrderGatewayShmVersion};
  std::uint16_t header_size{sizeof(OrderGatewayShmHeader)};
  std::uint16_t route_count{0};
  std::uint16_t reserved0{0};
  std::uint32_t command_queue_capacity{0};
  std::uint32_t event_queue_capacity{0};
  std::uint32_t startup_ready_timeout_s{30};
  std::uint32_t reserved1{0};
  OrderGatewayQueueDescriptor
      command_queue_descriptors[kMaxOrderGatewayRoutes]{};
  OrderGatewayQueueDescriptor event_queue_descriptors[kMaxOrderGatewayRoutes]{};
};

struct OrderGatewayCommand {
  std::uint64_t command_seq{0};
  std::uint64_t parent_id{0};
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::int64_t owner_enqueue_ns{0};

  std::int32_t symbol_id{0};
  std::uint16_t route_id{0};
  std::uint16_t symbol_size{0};
  std::uint16_t quantity_text_size{0};
  std::uint16_t price_text_size{0};

  OrderGatewayCommandKind kind{OrderGatewayCommandKind::kNone};
  Exchange exchange{Exchange::kGate};
  OrderSide side{OrderSide::kBuy};
  OrderType order_type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::uint8_t reduce_only{0};

  double quantity{0.0};

  char symbol[kOrderGatewaySymbolBytes]{};
  char quantity_text[kOrderGatewayQuantityTextBytes]{};
  char price_text[kOrderGatewayPriceTextBytes]{};
};

struct OrderGatewayEvent {
  std::uint64_t event_seq{0};
  std::uint64_t command_seq{0};
  std::uint64_t parent_id{0};
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t request_sequence{0};
  std::uint64_t encoded_request_id{0};

  std::int64_t worker_dequeue_ns{0};
  std::int64_t request_send_local_ns{0};
  std::int64_t local_receive_ns{0};
  std::int64_t exchange_ns{0};
  std::int64_t exchange_request_ingress_ns{0};
  std::int64_t exchange_response_egress_ns{0};
  std::int64_t exchange_process_ns{0};
  std::int64_t worker_event_enqueue_ns{0};

  std::uint16_t route_id{0};
  std::uint16_t http_status{0};

  OrderGatewayEventKind kind{OrderGatewayEventKind::kNone};
  OrderGatewayCommandKind command_kind{OrderGatewayCommandKind::kNone};
  OrderResponseKind response_kind{OrderResponseKind::kAck};
  OrderGatewayCommandRejectReason reject_reason{
      OrderGatewayCommandRejectReason::kNone};

  std::uint8_t ready{0};
};

}  // namespace aquila::core

#endif  // AQUILA_CORE_TRADING_ORDER_GATEWAY_SHM_TYPES_H_
