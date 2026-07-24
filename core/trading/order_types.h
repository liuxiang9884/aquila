#ifndef AQUILA_CORE_TRADING_ORDER_TYPES_H_
#define AQUILA_CORE_TRADING_ORDER_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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
  kUnknownResult,
  kCancelAccepted,
  kCancelRejected,
};

inline constexpr std::uint16_t kAutoGatewayRoute =
    static_cast<std::uint16_t>(0xFFFF);
inline constexpr std::uint16_t kInvalidOrderGroupIndex =
    std::numeric_limits<std::uint16_t>::max();
inline constexpr std::size_t kOrderSymbolBytes = 32;

struct OrderPlaceRequest {
  std::uint64_t local_order_id{0};
  std::uint64_t group_id{0};
  double price{0.0};
  double quantity{0.0};
  std::int32_t symbol_id{0};
  std::uint16_t gateway_route_id{kAutoGatewayRoute};
  Exchange exchange{Exchange::kGate};
  OrderSide side{OrderSide::kBuy};
  OrderType order_type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::uint8_t symbol_size{0};
  std::uint8_t price_decimal_places{0};
  std::uint8_t quantity_decimal_places{0};
  bool reduce_only{false};
  char symbol[kOrderSymbolBytes]{};

  [[nodiscard]] std::string_view SymbolView() const noexcept {
    return std::string_view(symbol, symbol_size);
  }
};

inline void SetOrderSymbol(OrderPlaceRequest* request,
                           std::string_view symbol) noexcept {
  // Precondition: validated instrument symbols fit in kOrderSymbolBytes.
  std::memcpy(request->symbol, symbol.data(), symbol.size());
  request->symbol_size = static_cast<std::uint8_t>(symbol.size());
}

struct OrderCancelRequest {
  std::uint64_t local_order_id{0};
  std::uint64_t group_id{0};
  std::uint16_t gateway_route_id{kAutoGatewayRoute};
};

struct StrategyOrder {
  OrderPlaceRequest place_request{};
  std::uint64_t exchange_order_id{0};
  std::uint16_t group_index{kInvalidOrderGroupIndex};
  OrderStatus status{OrderStatus::kCreated};
  OrderStatus pre_cancel_status{OrderStatus::kCreated};
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

struct OrderLocalMetadata {
  std::uint16_t group_index{kInvalidOrderGroupIndex};
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
  std::uint64_t group_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint16_t route_id{kAutoGatewayRoute};
  std::int64_t local_receive_ns{0};
  std::int64_t exchange_ns{0};
};

}  // namespace aquila::core

#endif  // AQUILA_CORE_TRADING_ORDER_TYPES_H_
