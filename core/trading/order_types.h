#ifndef AQUILA_CORE_TRADING_ORDER_TYPES_H_
#define AQUILA_CORE_TRADING_ORDER_TYPES_H_

#include <cstdint>
#include <string_view>

#include "core/common/types.h"
#include "core/trading/order_feedback_event.h"

namespace aquila::core {

enum class OrderStatus : std::uint8_t {
  kCreated,
  kSent,
  kAccepted,
  kPartialFilled,
  kFilled,
  kCancelSent,
  kCancelled,
  kPartiallyCancelled,
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
  OrderType order_type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  double quantity{0.0};
  std::string_view quantity_text{};
  std::string_view price_text{};
  bool reduce_only{false};
};

struct StrategyOrder {
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  double quantity{0.0};
  std::string_view quantity_text{};
  std::string_view price_text{};
  bool reduce_only{false};
  OrderStatus status{OrderStatus::kCreated};
  double cumulative_filled_quantity{0.0};
  double cumulative_filled_value{0.0};
  double last_fill_price{0.0};
  std::int64_t request_send_local_ns{0};
  std::int64_t ack_local_receive_ns{0};
  std::int64_t response_local_receive_ns{0};
  std::int64_t ack_exchange_ns{0};
  std::int64_t response_exchange_ns{0};
  std::int64_t accepted_exchange_ns{0};
  std::int64_t finish_exchange_ns{0};
  std::int64_t exchange_update_ns{0};
  OrderFinishReason finish_reason{OrderFinishReason::kUnknown};
  OrderRole role{OrderRole::kNone};
  OrderRejectReason reject_reason{OrderRejectReason::kUnknown};
  bool is_finished{false};

  [[nodiscard]] double AverageFillPrice() const noexcept {
    if (cumulative_filled_quantity <= 0.0) {
      return 0.0;
    }
    return cumulative_filled_value / cumulative_filled_quantity;
  }
};

struct StrategyFeedbackStats {
  std::uint64_t unknown_local_order_feedbacks{0};
  std::uint64_t duplicate_or_stale_feedbacks{0};
  std::uint64_t terminal_feedbacks_ignored{0};
  std::uint64_t feedback_continuity_lost_events{0};
};

struct OrderPlaceResult {
  OrderPlaceStatus status{OrderPlaceStatus::kInvalidOrder};
  std::uint64_t local_order_id{0};
};

struct OrderCancelResult {
  OrderCancelStatus status{OrderCancelStatus::kOrderNotFound};
  std::uint64_t local_order_id{0};
};

struct OrderResponseEvent {
  OrderResponseKind kind{OrderResponseKind::kAck};
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::int64_t local_receive_ns{0};
  std::int64_t exchange_ns{0};
};

}  // namespace aquila::core

#endif  // AQUILA_CORE_TRADING_ORDER_TYPES_H_
