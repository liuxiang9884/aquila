#ifndef AQUILA_CORE_TRADING_ORDER_FEEDBACK_EVENT_H_
#define AQUILA_CORE_TRADING_ORDER_FEEDBACK_EVENT_H_

#include <cstdint>
#include <type_traits>

namespace aquila {

enum class OrderFeedbackKind : std::uint8_t {
  kAccepted = 0,
  kPartialFilled = 1,
  kFilled = 2,
  kCancelled = 3,
  kRejected = 4,
  kContinuityLost = 5,
};

enum class OrderRole : std::uint8_t {
  kNone = 0,
  kMaker = 1,
  kTaker = 2,
};

enum class OrderFinishReason : std::uint8_t {
  kUnknown = 0,
  kManualCancelled = 1,
  kImmediateOrCancel = 2,
  kReduceOnly = 3,
  kReduceOut = 4,
  kSelfTradePrevention = 5,
  kLiquidated = 6,
  kAutoDeleveraging = 7,
  kPositionClose = 8,
};

enum class OrderRejectReason : std::uint8_t {
  kUnknown = 0,
  kSessionRejected = 1,
  kExchangeRejected = 2,
};

enum class OrderFeedbackContinuityScope : std::uint8_t {
  kLane = 0,
  kGlobal = 1,
};

enum class OrderFeedbackContinuityReason : std::uint8_t {
  kUnknown = 0,
  kLaneQueueFull = 1,
  kSessionDisconnected = 2,
  kReconnectUnknownWindow = 3,
  kDecodeUnrecoverable = 4,
  kProducerRestart = 5,
};

struct OrderFeedbackEvent {
  OrderFeedbackKind kind;
  std::uint64_t local_order_id;
  std::uint64_t exchange_order_id;
  std::int64_t cumulative_filled_quantity;
  std::int64_t left_quantity;
  std::int64_t cancelled_quantity;
  double fill_price;
  OrderRole role;
  OrderFinishReason finish_reason;
  OrderRejectReason reject_reason;
  OrderFeedbackContinuityScope continuity_scope;
  OrderFeedbackContinuityReason continuity_reason;
  std::uint64_t continuity_sequence;
  std::int64_t exchange_update_ns;
  std::int64_t local_receive_ns;
};

static_assert(std::is_trivial_v<OrderFeedbackEvent>);
static_assert(std::is_standard_layout_v<OrderFeedbackEvent>);
static_assert(sizeof(OrderFeedbackEvent) == 88);

}  // namespace aquila

#endif  // AQUILA_CORE_TRADING_ORDER_FEEDBACK_EVENT_H_
