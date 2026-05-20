#include "core/trading/order_feedback_event.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "core/trading/order_id.h"

namespace aquila {
namespace {

TEST(OrderFeedbackEventTest, IsTrivialAbiCarrier) {
  static_assert(std::is_trivial_v<OrderFeedbackEvent>);
  EXPECT_TRUE(std::is_trivial_v<OrderFeedbackEvent>);
}

TEST(OrderFeedbackEventTest, IsStandardLayoutAbiCarrier) {
  static_assert(std::is_standard_layout_v<OrderFeedbackEvent>);
  EXPECT_TRUE(std::is_standard_layout_v<OrderFeedbackEvent>);
}

TEST(OrderFeedbackEventTest, IsDefaultConstructibleAbiCarrier) {
  static_assert(std::is_default_constructible_v<OrderFeedbackEvent>);
  EXPECT_TRUE(std::is_default_constructible_v<OrderFeedbackEvent>);
}

TEST(OrderFeedbackEventTest, ValueInitializationUsesAbiDefaults) {
  const OrderFeedbackEvent event{};

  EXPECT_EQ(event.kind, OrderFeedbackKind::kAccepted);
  EXPECT_EQ(event.local_order_id, 0U);
  EXPECT_EQ(event.continuity_scope, OrderFeedbackContinuityScope::kLane);
  EXPECT_EQ(event.continuity_reason, OrderFeedbackContinuityReason::kUnknown);
  EXPECT_EQ(event.continuity_sequence, 0U);
}

TEST(OrderFeedbackEventTest,
     ContinuityLostEventCanDescribeGlobalReconnectUnknownWindow) {
  const OrderFeedbackEvent event{
      .kind = OrderFeedbackKind::kContinuityLost,
      .continuity_scope = OrderFeedbackContinuityScope::kGlobal,
      .continuity_reason =
          OrderFeedbackContinuityReason::kReconnectUnknownWindow,
      .continuity_sequence = 7,
      .local_receive_ns = 123,
  };

  EXPECT_EQ(event.kind, OrderFeedbackKind::kContinuityLost);
  EXPECT_EQ(event.local_order_id, 0U);
  EXPECT_EQ(event.continuity_scope, OrderFeedbackContinuityScope::kGlobal);
  EXPECT_EQ(event.continuity_reason,
            OrderFeedbackContinuityReason::kReconnectUnknownWindow);
  EXPECT_EQ(event.continuity_sequence, 7U);
  EXPECT_EQ(event.local_receive_ns, 123);
}

TEST(OrderFeedbackEventTest, ContinuityLostEventCarriesNoLocalOrderId) {
  const OrderFeedbackEvent event{
      .kind = OrderFeedbackKind::kContinuityLost,
      .continuity_scope = OrderFeedbackContinuityScope::kLane,
      .continuity_reason = OrderFeedbackContinuityReason::kLaneQueueFull,
      .continuity_sequence = 9,
  };

  EXPECT_EQ(event.kind, OrderFeedbackKind::kContinuityLost);
  EXPECT_EQ(event.local_order_id, 0U);
}

TEST(OrderFeedbackEventTest, ContinuousOrderEventLocalOrderIdRoutesToStrategy) {
  constexpr std::uint64_t local_order_id = LocalOrderIdCodec::Encode(3, 42);
  const OrderFeedbackEvent event{
      .kind = OrderFeedbackKind::kAccepted,
      .local_order_id = local_order_id,
  };

  EXPECT_NE(event.kind, OrderFeedbackKind::kContinuityLost);
  EXPECT_EQ(LocalOrderIdCodec::StrategyId(event.local_order_id), 3);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(event.local_order_id), 42U);
}

TEST(OrderFeedbackEventTest, SizeIsFixedForShmAbi) {
  static_assert(sizeof(OrderFeedbackEvent) == 88);
  EXPECT_EQ(sizeof(OrderFeedbackEvent), 88U);
}

TEST(OrderFeedbackEventTest, AlignmentIsFixedForShmAbi) {
  static_assert(alignof(OrderFeedbackEvent) == 8);
  EXPECT_EQ(alignof(OrderFeedbackEvent), 8U);
}

TEST(OrderFeedbackEventTest, FieldOffsetsAreFixedForShmAbi) {
  static_assert(offsetof(OrderFeedbackEvent, kind) == 0);
  static_assert(offsetof(OrderFeedbackEvent, local_order_id) == 8);
  static_assert(offsetof(OrderFeedbackEvent, exchange_order_id) == 16);
  static_assert(offsetof(OrderFeedbackEvent, cumulative_filled_quantity) == 24);
  static_assert(offsetof(OrderFeedbackEvent, fill_price) == 48);
  static_assert(offsetof(OrderFeedbackEvent, role) == 56);
  static_assert(offsetof(OrderFeedbackEvent, continuity_scope) == 59);
  static_assert(offsetof(OrderFeedbackEvent, continuity_reason) == 60);
  static_assert(offsetof(OrderFeedbackEvent, continuity_sequence) == 64);
  static_assert(offsetof(OrderFeedbackEvent, exchange_update_ns) == 72);
  static_assert(offsetof(OrderFeedbackEvent, local_receive_ns) == 80);
}

TEST(OrderFeedbackEventTest, EnumValuesAreFixedForShmAbi) {
  static_assert(static_cast<std::uint8_t>(OrderFeedbackKind::kAccepted) == 0);
  static_assert(static_cast<std::uint8_t>(OrderFeedbackKind::kContinuityLost) ==
                5);
  static_assert(
      static_cast<std::uint8_t>(OrderFeedbackContinuityScope::kGlobal) == 1);
  static_assert(static_cast<std::uint8_t>(
                    OrderFeedbackContinuityReason::kReconnectUnknownWindow) ==
                3);

  EXPECT_EQ(static_cast<std::uint8_t>(OrderFeedbackKind::kAccepted), 0U);
  EXPECT_EQ(static_cast<std::uint8_t>(OrderFeedbackKind::kContinuityLost), 5U);
  EXPECT_EQ(static_cast<std::uint8_t>(OrderFeedbackContinuityScope::kGlobal),
            1U);
  EXPECT_EQ(static_cast<std::uint8_t>(
                OrderFeedbackContinuityReason::kReconnectUnknownWindow),
            3U);
}

}  // namespace
}  // namespace aquila
