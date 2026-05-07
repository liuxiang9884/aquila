#ifndef AQUILA_STRATEGY_ORDER_TYPES_H_
#define AQUILA_STRATEGY_ORDER_TYPES_H_

#include <cstdint>
#include <string_view>

#include "core/common/types.h"

namespace aquila::strategy {

enum class OrderSide : std::uint8_t { kBuy, kSell };
enum class OrderType : std::uint8_t { kLimit, kMarket };
enum class TimeInForce : std::uint8_t { kGoodTillCancel, kImmediateOrCancel };

enum class OrderStatus : std::uint8_t {
  kCreated,
  kSubmitted,
  kAcked,
  kAccepted,
  kRejected,
  kCancelSubmitted,
  kCancelAccepted,
  kCancelRejected,
};

enum class GatewaySendStatus : std::uint8_t { kOk, kRejected };

enum class OrderCreateStatus : std::uint8_t {
  kOk,
  kInvalidOrder,
  kStoreFull,
  kGatewayRejected,
};

enum class OrderSubmitStatus : std::uint8_t {
  kOk,
  kOrderNotFound,
  kInvalidStatus,
  kGatewayRejected,
};

enum class OrderCancelStatus : std::uint8_t {
  kOk,
  kOrderNotFound,
  kInvalidStatus,
  kGatewayRejected,
};

enum class OrderResponseKind : std::uint8_t {
  kAck,
  kAccepted,
  kRejected,
  kCancelAccepted,
  kCancelRejected,
};

struct GatewaySendResult {
  GatewaySendStatus status{GatewaySendStatus::kRejected};
};

struct OrderDraft {
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::int64_t signed_quantity{0};
  std::string_view price_text{};
  bool reduce_only{false};
};

struct StrategyOrder {
  std::int64_t local_order_id{0};
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::int64_t signed_quantity{0};
  std::uint64_t exchange_order_id{0};
  bool reduce_only{false};
  OrderStatus status{OrderStatus::kCreated};
  std::uint64_t error_label_hash{0};
};

struct OrderCreateResult {
  OrderCreateStatus status{OrderCreateStatus::kInvalidOrder};
  std::int64_t local_order_id{0};
};

struct OrderSubmitResult {
  OrderSubmitStatus status{OrderSubmitStatus::kOrderNotFound};
  std::int64_t local_order_id{0};
};

struct OrderCancelResult {
  OrderCancelStatus status{OrderCancelStatus::kOrderNotFound};
  std::int64_t local_order_id{0};
};

struct OrderResponseEvent {
  OrderResponseKind kind{OrderResponseKind::kAck};
  std::int64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t error_label_hash{0};
};

}  // namespace aquila::strategy

#endif  // AQUILA_STRATEGY_ORDER_TYPES_H_
