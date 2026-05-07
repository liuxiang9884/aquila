#ifndef AQUILA_STRATEGY_ORDER_TYPES_H_
#define AQUILA_STRATEGY_ORDER_TYPES_H_

#include <cstdint>
#include <string_view>

#include "core/common/types.h"

namespace aquila::strategy {

enum class OrderStatus : std::uint8_t {
  kCreated,
  kSent,
  kAccepted,
  kPartialFilled,
  kFilled,
  kCancelSent,
  kCancelled,
  kRejected,
};

enum class OrderPlaceStatus : std::uint8_t {
  kOk,
  kInvalidOrder,
  kPoolFull,
  kSessionRejected,
};

enum class OrderCancelStatus : std::uint8_t {
  kOk,
  kOrderNotFound,
  kInvalidStatus,
  kSessionRejected,
};

enum class OrderResponseKind : std::uint8_t {
  kAck,
  kAccepted,
  kRejected,
  kCancelAccepted,
  kCancelRejected,
};

struct OrderCreateRequest {
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::int64_t quantity{0};
  std::string_view price_text{};
  bool reduce_only{false};
};

struct StrategyOrder {
  std::int64_t local_order_id{0};
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::int64_t quantity{0};
  std::string_view price_text{};
  std::uint64_t exchange_order_id{0};
  bool reduce_only{false};
  OrderStatus status{OrderStatus::kCreated};
  std::uint64_t error_label_hash{0};
};

struct OrderPlaceResult {
  OrderPlaceStatus status{OrderPlaceStatus::kInvalidOrder};
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
