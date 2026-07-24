#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_manager.h"
#include "core/trading/order_types.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/execution_state.h"
#include "strategy/lead_lag/signal.h"
#include "strategy/lead_lag/threshold.h"

namespace {

namespace leadlag = aquila::strategy::leadlag;

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kOk};
  };

  SendResult PlaceOrder(const aquila::core::OrderPlaceRequest&) noexcept {
    return {};
  }

  SendResult CancelOrder(const aquila::core::OrderCancelRequest&) noexcept {
    return {};
  }
};

leadlag::InstrumentMetadata Instrument() {
  return leadlag::InstrumentMetadata{
      .quantity_step = 1.0,
      .notional_multiplier = 1.0,
  };
}

aquila::core::StrategyOrder Order(std::uint64_t local_order_id,
                                  aquila::OrderSide side,
                                  std::int64_t cumulative_filled_quantity,
                                  double fill_price) {
  return aquila::core::StrategyOrder{
      .place_request =
          aquila::core::OrderPlaceRequest{
              .local_order_id = local_order_id,
              .quantity = static_cast<double>(cumulative_filled_quantity),
              .side = side,
          },
      .status = aquila::core::OrderStatus::kFilled,
      .cumulative_filled_quantity =
          static_cast<double>(cumulative_filled_quantity),
      .cumulative_filled_value =
          static_cast<double>(cumulative_filled_quantity) * fill_price,
      .last_fill_price = fill_price,
      .is_finished = true,
  };
}

void StampOrderGroup(const leadlag::ExecutionGroup& group,
                     aquila::core::StrategyOrder* order) {
  order->place_request.group_id = group.group_id;
  order->group_index = group.group_index;
}

leadlag::PairConfig PairConfigForFeedback() {
  leadlag::PairConfig pair;
  pair.symbol_id = 1;
  pair.lag_exchange = aquila::Exchange::kGate;
  pair.lag_taker_fee = 0.0001;
  pair.trigger = leadlag::TriggerConfig{
      .lead = 0.02,
      .close = 0.005,
      .lag_part = 0.5,
  };
  pair.execute = leadlag::ExecuteConfig{
      .trailing_stop = 0.05,
      .max_entry_spread = 0.02,
      .parallel = 1,
  };
  return pair;
}

leadlag::SignalMarket OpenLongMarketForFeedback() {
  return leadlag::SignalMarket{
      .lead =
          leadlag::QuoteSnapshot{
              .event_ns = 10,
              .bid_price = 104.0,
              .ask_price = 105.0,
          },
      .lag =
          leadlag::QuoteSnapshot{
              .event_ns = 10,
              .bid_price = 101.5,
              .ask_price = 102.0,
          },
      .recorder =
          leadlag::RecorderSnapshot{
              .lead_extrema =
                  leadlag::BboExtremaSnapshot{
                      .valid = true,
                      .bid_min = 100.0,
                      .bid_max = 104.0,
                      .ask_min = 101.0,
                      .ask_max = 105.0,
                  },
              .lag_extrema =
                  leadlag::BboExtremaSnapshot{
                      .valid = true,
                      .bid_min = 101.5,
                      .bid_max = 102.0,
                      .ask_min = 102.0,
                      .ask_max = 102.5,
                  },
              .lag_spread_mean = 0.4,
          },
  };
}

leadlag::ThresholdSnapshot ThresholdForFeedback() {
  return leadlag::ThresholdSnapshot{
      .initialized = true,
      .up_entry = 0.02,
      .down_entry = -0.02,
      .up_exit = 0.005,
      .down_exit = -0.005,
  };
}

TEST(LeadLagFeedbackStateTest, OrderManagerRetiresOnlyFinishedOrders) {
  FakeOrderSession session;
  aquila::core::OrderManager<FakeOrderSession> manager(session, 2, 1);

  aquila::core::OrderPlaceRequest request{
      .price = 100.0,
      .quantity = 1,
      .time_in_force = aquila::TimeInForce::kImmediateOrCancel,
      .price_decimal_places = 1,
      .quantity_decimal_places = 0,
  };
  aquila::core::SetOrderSymbol(&request, "BTC_USDT");
  const aquila::core::OrderPlaceResult placed =
      manager.PlaceLimitOrder(request);

  ASSERT_EQ(placed.status, aquila::core::OrderPlaceStatus::kOk);
  EXPECT_FALSE(manager.RetireFinishedOrder(placed.local_order_id));
  ASSERT_NE(manager.FindOrder(placed.local_order_id), nullptr);

  manager.FindOrder(placed.local_order_id)->is_finished = true;
  EXPECT_TRUE(manager.RetireFinishedOrder(placed.local_order_id));
  EXPECT_EQ(manager.FindOrder(placed.local_order_id), nullptr);
  EXPECT_EQ(manager.order_count(), 0U);
  EXPECT_FALSE(manager.RetireFinishedOrder(placed.local_order_id));
}

TEST(LeadLagFeedbackStateTest, OpenTerminalFeedbackMovesGroupToHold) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/1);
  const leadlag::ExecutionGroup* group =
      state.StartOpenOrder(/*local_order_id=*/11);
  ASSERT_NE(group, nullptr);
  aquila::core::StrategyOrder order =
      Order(/*local_order_id=*/11, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/3, /*fill_price=*/102.0);
  StampOrderGroup(*group, &order);

  const leadlag::ExecutionApplyResult result =
      state.ApplyTerminalOrder(order, Instrument());

  EXPECT_EQ(result, leadlag::ExecutionApplyResult::kAppliedHold);
  ASSERT_EQ(state.active_group_count(), 1U);
  const leadlag::ExecutionGroup* updated = state.FindGroupById(1);
  ASSERT_NE(updated, nullptr);
  EXPECT_EQ(updated->stage, leadlag::ExecutionStage::kHold);
  EXPECT_EQ(updated->signed_position_quantity, 3);
  EXPECT_EQ(updated->local_order_id, 0U);
  EXPECT_DOUBLE_EQ(updated->trailing_price, 102.0);
}

TEST(LeadLagFeedbackStateTest, SubmitRejectedDeletesEmptyOpenGroup) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/2);
  const leadlag::ExecutionGroup* group =
      state.StartOpenOrder(/*local_order_id=*/21);
  ASSERT_NE(group, nullptr);
  aquila::core::StrategyOrder rejected =
      Order(/*local_order_id=*/21, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/0, /*fill_price=*/0.0);
  rejected.status = aquila::core::OrderStatus::kRejected;
  StampOrderGroup(*group, &rejected);

  EXPECT_EQ(state.ApplySubmitRejected(rejected),
            leadlag::ExecutionApplyResult::kAppliedDeleted);
  EXPECT_EQ(state.active_group_count(), 0U);
  EXPECT_EQ(state.ApplySubmitRejected(aquila::core::StrategyOrder{}),
            leadlag::ExecutionApplyResult::kIgnoredGroupMismatch);
  EXPECT_EQ(state.ApplySubmitRejected(rejected),
            leadlag::ExecutionApplyResult::kIgnoredGroupMismatch);
}

TEST(LeadLagFeedbackStateTest, CloseTerminalFeedbackDeletesFlatGroup) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/1);
  leadlag::ExecutionGroup* group =
      state.AddHoldGroup(/*signed_position_quantity=*/3,
                         /*trailing_price=*/102.0);
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(state.StartCloseOrder(*group, /*local_order_id=*/12));
  aquila::core::StrategyOrder order =
      Order(/*local_order_id=*/12, aquila::OrderSide::kSell,
            /*cumulative_filled_quantity=*/3, /*fill_price=*/101.0);
  StampOrderGroup(*group, &order);

  const leadlag::ExecutionApplyResult result =
      state.ApplyTerminalOrder(order, Instrument());

  EXPECT_EQ(result, leadlag::ExecutionApplyResult::kAppliedDeleted);
  EXPECT_EQ(state.active_group_count(), 0U);
  EXPECT_EQ(state.FindGroupById(1), nullptr);
}

TEST(LeadLagFeedbackStateTest, RejectedCloseReturnsExistingPositionToHold) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/1);
  leadlag::ExecutionGroup* group =
      state.AddHoldGroup(/*signed_position_quantity=*/3,
                         /*trailing_price=*/102.0);
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(state.StartCloseOrder(*group, /*local_order_id=*/13));

  aquila::core::StrategyOrder rejected =
      Order(/*local_order_id=*/13, aquila::OrderSide::kSell,
            /*cumulative_filled_quantity=*/0, /*fill_price=*/0.0);
  rejected.status = aquila::core::OrderStatus::kRejected;
  StampOrderGroup(*group, &rejected);

  const leadlag::ExecutionApplyResult result =
      state.ApplyTerminalOrder(rejected, Instrument());

  EXPECT_EQ(result, leadlag::ExecutionApplyResult::kAppliedHold);
  ASSERT_EQ(state.active_group_count(), 1U);
  const leadlag::ExecutionGroup* updated = state.FindGroupById(1);
  ASSERT_NE(updated, nullptr);
  EXPECT_EQ(updated->stage, leadlag::ExecutionStage::kHold);
  EXPECT_EQ(updated->signed_position_quantity, 3);
  EXPECT_EQ(updated->local_order_id, 0U);
}

TEST(LeadLagFeedbackStateTest, NormalCloseTerminalFailureIncrementsRetryCount) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/1);
  leadlag::ExecutionGroup* group =
      state.AddHoldGroup(/*signed_position_quantity=*/3,
                         /*trailing_price=*/102.0);
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(state.StartCloseOrder(*group, /*local_order_id=*/13,
                                    leadlag::CloseOrderKind::kNormal));
  aquila::core::StrategyOrder order =
      Order(/*local_order_id=*/13, aquila::OrderSide::kSell,
            /*cumulative_filled_quantity=*/0, /*fill_price=*/0.0);
  StampOrderGroup(*group, &order);

  const leadlag::ExecutionApplyResult result =
      state.ApplyTerminalOrder(order, Instrument());

  EXPECT_EQ(result, leadlag::ExecutionApplyResult::kAppliedHold);
  const leadlag::ExecutionGroup* updated = state.FindGroupById(1);
  ASSERT_NE(updated, nullptr);
  EXPECT_EQ(updated->normal_close_retry_count, 1U);
  EXPECT_TRUE(updated->CanSubmitNormalClose(/*close_retry_times=*/1));
  EXPECT_FALSE(updated->CanSubmitNormalClose(/*close_retry_times=*/0));
}

TEST(LeadLagFeedbackStateTest,
     StoplossTerminalFailureDoesNotIncrementRetryCount) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/1);
  leadlag::ExecutionGroup* group =
      state.AddHoldGroup(/*signed_position_quantity=*/3,
                         /*trailing_price=*/102.0);
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(state.StartCloseOrder(*group, /*local_order_id=*/13,
                                    leadlag::CloseOrderKind::kStoploss));
  aquila::core::StrategyOrder order =
      Order(/*local_order_id=*/13, aquila::OrderSide::kSell,
            /*cumulative_filled_quantity=*/0, /*fill_price=*/0.0);
  StampOrderGroup(*group, &order);

  const leadlag::ExecutionApplyResult result =
      state.ApplyTerminalOrder(order, Instrument());

  EXPECT_EQ(result, leadlag::ExecutionApplyResult::kAppliedHold);
  const leadlag::ExecutionGroup* updated = state.FindGroupById(1);
  ASSERT_NE(updated, nullptr);
  EXPECT_EQ(updated->normal_close_retry_count, 0U);
}

TEST(LeadLagFeedbackStateTest, RejectedNormalCloseSubmitIncrementsRetryCount) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/1);
  leadlag::ExecutionGroup* group =
      state.AddHoldGroup(/*signed_position_quantity=*/3,
                         /*trailing_price=*/102.0);
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(state.StartCloseOrder(*group, /*local_order_id=*/13,
                                    leadlag::CloseOrderKind::kNormal));
  aquila::core::StrategyOrder rejected =
      Order(/*local_order_id=*/13, aquila::OrderSide::kSell,
            /*cumulative_filled_quantity=*/0, /*fill_price=*/0.0);
  rejected.status = aquila::core::OrderStatus::kRejected;
  StampOrderGroup(*group, &rejected);

  EXPECT_EQ(state.ApplySubmitRejected(rejected),
            leadlag::ExecutionApplyResult::kAppliedHold);
  const leadlag::ExecutionGroup* updated = state.FindGroupById(1);
  ASSERT_NE(updated, nullptr);
  EXPECT_EQ(updated->normal_close_retry_count, 1U);
}

TEST(LeadLagFeedbackStateTest, ClearGroupByIdRemovesActiveGroup) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/2);
  leadlag::ExecutionGroup* first =
      state.AddHoldGroup(/*signed_position_quantity=*/3,
                         /*trailing_price=*/102.0);
  leadlag::ExecutionGroup* second =
      state.AddHoldGroup(/*signed_position_quantity=*/-2,
                         /*trailing_price=*/99.0);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  const std::uint64_t first_group_id = first->group_id;
  const std::uint64_t second_group_id = second->group_id;
  ASSERT_EQ(state.active_group_count(), 2U);

  EXPECT_TRUE(state.ClearGroupById(second_group_id));
  EXPECT_EQ(state.active_group_count(), 1U);
  EXPECT_EQ(state.FindGroupById(second_group_id), nullptr);
  EXPECT_NE(state.FindGroupById(first_group_id), nullptr);
  EXPECT_FALSE(state.ClearGroupById(second_group_id));
  EXPECT_EQ(state.active_group_count(), 1U);
}

TEST(LeadLagFeedbackStateTest, RuntimeCapacityRejectsOutOfRangeParallel) {
  leadlag::ExecutionState state;

  EXPECT_TRUE(state.Init(/*parallel=*/16));
  EXPECT_EQ(state.capacity(), 16U);
  EXPECT_FALSE(state.Init(/*parallel=*/17));
  EXPECT_EQ(state.capacity(), 0U);
  EXPECT_FALSE(state.Init(/*parallel=*/0));
  EXPECT_EQ(state.capacity(), 0U);
}

TEST(LeadLagFeedbackStateTest, ReusedSlotRequiresMatchingGroupId) {
  leadlag::ExecutionState state;
  ASSERT_TRUE(state.Init(/*parallel=*/2));
  leadlag::ExecutionGroup* first = state.StartOpenGroup();
  ASSERT_NE(first, nullptr);
  const std::uint64_t first_group_id = first->group_id;
  const std::uint16_t first_group_index = first->group_index;
  ASSERT_TRUE(state.ClearGroupById(first_group_id));

  leadlag::ExecutionGroup* second = state.StartOpenGroup();

  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->group_index, first_group_index);
  EXPECT_NE(second->group_id, first_group_id);
  EXPECT_EQ(state.GroupAt(first_group_index, first_group_id), nullptr);
  EXPECT_EQ(state.GroupAt(first_group_index, second->group_id), second);
}

TEST(LeadLagFeedbackStateTest,
     ParallelCapacityRejectsUntilClearedSlotIsReusedWithNewId) {
  leadlag::ExecutionState state;
  ASSERT_TRUE(state.Init(/*parallel=*/2));
  leadlag::ExecutionGroup* first = state.StartOpenGroup();
  leadlag::ExecutionGroup* second = state.StartOpenGroup();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  const std::uint64_t first_group_id = first->group_id;
  const std::uint64_t second_group_id = second->group_id;
  const leadlag::ExecutionGroupIndex first_group_index = first->group_index;

  EXPECT_EQ(state.StartOpenGroup(), nullptr);
  EXPECT_EQ(state.active_group_count(), 2U);
  ASSERT_TRUE(state.ClearGroupById(first_group_id));
  leadlag::ExecutionGroup* replacement = state.StartOpenGroup();
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(replacement->group_index, first_group_index);
  EXPECT_GT(replacement->group_id, second_group_id);
  EXPECT_NE(replacement->group_id, first_group_id);
  EXPECT_EQ(state.active_group_count(), 2U);
}

TEST(LeadLagFeedbackStateTest, MismatchedTerminalOrderDoesNotMutateReusedSlot) {
  leadlag::ExecutionState state;
  ASSERT_TRUE(state.Init(/*parallel=*/2));
  const leadlag::ExecutionGroup* first = state.StartOpenOrder(31);
  ASSERT_NE(first, nullptr);
  aquila::core::StrategyOrder stale =
      Order(31, aquila::OrderSide::kBuy, 1, 100.0);
  StampOrderGroup(*first, &stale);
  const std::uint64_t first_group_id = first->group_id;
  ASSERT_TRUE(state.ClearGroupById(first_group_id));
  leadlag::ExecutionGroup* second = state.StartOpenOrder(32);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(second->group_index, stale.group_index);

  const leadlag::ExecutionApplyResult result =
      state.ApplyTerminalOrder(stale, Instrument());

  EXPECT_EQ(result, leadlag::ExecutionApplyResult::kIgnoredGroupMismatch);
  EXPECT_EQ(state.FindGroupById(second->group_id), second);
  EXPECT_EQ(second->pending_order_count, 1U);
  EXPECT_EQ(second->pending_local_order_ids[0], 32U);
}

TEST(LeadLagFeedbackStateTest, ParallelGroupsApplyTerminalOrdersIndependently) {
  leadlag::ExecutionState state;
  ASSERT_TRUE(state.Init(/*parallel=*/2));
  const leadlag::ExecutionGroup* first = state.StartOpenOrder(41);
  const leadlag::ExecutionGroup* second = state.StartOpenOrder(42);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  const std::uint64_t first_group_id = first->group_id;
  const std::uint64_t second_group_id = second->group_id;
  aquila::core::StrategyOrder second_rejected =
      Order(42, aquila::OrderSide::kBuy, 0, 0.0);
  second_rejected.status = aquila::core::OrderStatus::kRejected;
  StampOrderGroup(*second, &second_rejected);

  EXPECT_EQ(state.ApplyTerminalOrder(second_rejected, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedDeleted);
  EXPECT_NE(state.FindGroupById(first_group_id), nullptr);
  EXPECT_EQ(state.FindGroupById(second_group_id), nullptr);

  aquila::core::StrategyOrder first_filled =
      Order(41, aquila::OrderSide::kBuy, 2, 100.0);
  StampOrderGroup(*state.FindGroupById(first_group_id), &first_filled);
  EXPECT_EQ(state.ApplyTerminalOrder(first_filled, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedHold);
  ASSERT_NE(state.FindGroupById(first_group_id), nullptr);
  EXPECT_DOUBLE_EQ(
      state.FindGroupById(first_group_id)->signed_position_quantity, 2.0);
}

TEST(LeadLagFeedbackStateTest,
     ParallelFanoutTerminalsRemainIsolatedWhenDeliveredOutOfOrder) {
  leadlag::ExecutionState state;
  ASSERT_TRUE(state.Init(/*parallel=*/2));
  leadlag::ExecutionGroup* first = state.StartOpenGroup();
  leadlag::ExecutionGroup* second = state.StartOpenGroup();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(state.AddOpenOrder(*first, /*local_order_id=*/51));
  ASSERT_TRUE(state.AddOpenOrder(*first, /*local_order_id=*/52));
  ASSERT_TRUE(state.AddOpenOrder(*second, /*local_order_id=*/61));
  ASSERT_TRUE(state.AddOpenOrder(*second, /*local_order_id=*/62));
  const std::uint64_t first_group_id = first->group_id;
  const std::uint64_t second_group_id = second->group_id;

  aquila::core::StrategyOrder second_partial =
      Order(/*local_order_id=*/62, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/1, /*fill_price=*/200.0);
  second_partial.status = aquila::core::OrderStatus::kPartiallyCancelled;
  StampOrderGroup(*second, &second_partial);
  EXPECT_EQ(state.ApplyTerminalOrder(second_partial, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedHold);
  const leadlag::ExecutionGroup* updated_second =
      state.FindGroupById(second_group_id);
  ASSERT_NE(updated_second, nullptr);
  EXPECT_EQ(updated_second->stage, leadlag::ExecutionStage::kOpen);
  EXPECT_EQ(updated_second->pending_order_count, 1U);
  EXPECT_DOUBLE_EQ(updated_second->signed_position_quantity, 1.0);
  EXPECT_EQ(state.ApplyTerminalOrder(second_partial, Instrument()),
            leadlag::ExecutionApplyResult::kIgnoredUnknownOrder);
  EXPECT_DOUBLE_EQ(
      state.FindGroupById(second_group_id)->signed_position_quantity, 1.0);

  aquila::core::StrategyOrder first_cancelled =
      Order(/*local_order_id=*/52, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/0, /*fill_price=*/0.0);
  first_cancelled.status = aquila::core::OrderStatus::kCancelled;
  StampOrderGroup(*first, &first_cancelled);
  EXPECT_EQ(state.ApplyTerminalOrder(first_cancelled, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedHold);
  EXPECT_EQ(state.FindGroupById(first_group_id)->pending_order_count, 1U);
  EXPECT_DOUBLE_EQ(
      state.FindGroupById(first_group_id)->signed_position_quantity, 0.0);

  aquila::core::StrategyOrder second_filled =
      Order(/*local_order_id=*/61, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/3, /*fill_price=*/201.0);
  StampOrderGroup(*state.FindGroupById(second_group_id), &second_filled);
  EXPECT_EQ(state.ApplyTerminalOrder(second_filled, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedHold);
  updated_second = state.FindGroupById(second_group_id);
  ASSERT_NE(updated_second, nullptr);
  EXPECT_EQ(updated_second->stage, leadlag::ExecutionStage::kHold);
  EXPECT_DOUBLE_EQ(updated_second->signed_position_quantity, 4.0);

  aquila::core::StrategyOrder first_filled =
      Order(/*local_order_id=*/51, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/2, /*fill_price=*/100.0);
  StampOrderGroup(*state.FindGroupById(first_group_id), &first_filled);
  EXPECT_EQ(state.ApplyTerminalOrder(first_filled, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedHold);
  const leadlag::ExecutionGroup* updated_first =
      state.FindGroupById(first_group_id);
  ASSERT_NE(updated_first, nullptr);
  EXPECT_EQ(updated_first->stage, leadlag::ExecutionStage::kHold);
  EXPECT_DOUBLE_EQ(updated_first->signed_position_quantity, 2.0);
  EXPECT_DOUBLE_EQ(
      state.FindGroupById(second_group_id)->signed_position_quantity, 4.0);
}

TEST(LeadLagFeedbackStateTest,
     ActiveGroupIdMismatchDoesNotFallbackToAnotherSlot) {
  leadlag::ExecutionState state;
  ASSERT_TRUE(state.Init(/*parallel=*/2));
  const leadlag::ExecutionGroup* first = state.StartOpenOrder(71);
  const leadlag::ExecutionGroup* second = state.StartOpenOrder(72);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  const std::uint64_t first_group_id = first->group_id;
  const std::uint64_t second_group_id = second->group_id;
  aquila::core::StrategyOrder mismatched =
      Order(/*local_order_id=*/71, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/1, /*fill_price=*/100.0);
  mismatched.group_index = first->group_index;
  mismatched.place_request.group_id = second_group_id;

  EXPECT_EQ(state.ApplyTerminalOrder(mismatched, Instrument()),
            leadlag::ExecutionApplyResult::kIgnoredGroupMismatch);
  const leadlag::ExecutionGroup* unchanged_first =
      state.FindGroupById(first_group_id);
  const leadlag::ExecutionGroup* unchanged_second =
      state.FindGroupById(second_group_id);
  ASSERT_NE(unchanged_first, nullptr);
  ASSERT_NE(unchanged_second, nullptr);
  EXPECT_EQ(unchanged_first->pending_order_count, 1U);
  EXPECT_EQ(unchanged_first->pending_local_order_ids[0], 71U);
  EXPECT_DOUBLE_EQ(unchanged_first->signed_position_quantity, 0.0);
  EXPECT_EQ(unchanged_second->pending_order_count, 1U);
  EXPECT_EQ(unchanged_second->pending_local_order_ids[0], 72U);
  EXPECT_DOUBLE_EQ(unchanged_second->signed_position_quantity, 0.0);
}

TEST(LeadLagFeedbackStateTest,
     CloseRetryAndStoplossCompletionRemainIsolatedPerGroup) {
  leadlag::ExecutionState state;
  ASSERT_TRUE(state.Init(/*parallel=*/2));
  leadlag::ExecutionGroup* long_group =
      state.AddHoldGroup(/*signed_position_quantity=*/5,
                         /*trailing_price=*/100.0);
  leadlag::ExecutionGroup* short_group =
      state.AddHoldGroup(/*signed_position_quantity=*/-4,
                         /*trailing_price=*/200.0);
  ASSERT_NE(long_group, nullptr);
  ASSERT_NE(short_group, nullptr);
  const std::uint64_t long_group_id = long_group->group_id;
  const std::uint64_t short_group_id = short_group->group_id;
  ASSERT_TRUE(state.StartCloseOrder(*long_group, /*local_order_id=*/81,
                                    leadlag::CloseOrderKind::kNormal));
  ASSERT_TRUE(state.StartCloseOrder(*short_group, /*local_order_id=*/82,
                                    leadlag::CloseOrderKind::kStoploss));

  aquila::core::StrategyOrder short_closed =
      Order(/*local_order_id=*/82, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/4, /*fill_price=*/199.0);
  short_closed.place_request.reduce_only = true;
  StampOrderGroup(*short_group, &short_closed);
  EXPECT_EQ(state.ApplyTerminalOrder(short_closed, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedDeleted);
  EXPECT_EQ(state.FindGroupById(short_group_id), nullptr);
  const leadlag::ExecutionGroup* unchanged_long =
      state.FindGroupById(long_group_id);
  ASSERT_NE(unchanged_long, nullptr);
  EXPECT_EQ(unchanged_long->stage, leadlag::ExecutionStage::kClose);
  EXPECT_DOUBLE_EQ(unchanged_long->signed_position_quantity, 5.0);
  EXPECT_EQ(unchanged_long->normal_close_retry_count, 0U);

  aquila::core::StrategyOrder long_cancelled =
      Order(/*local_order_id=*/81, aquila::OrderSide::kSell,
            /*cumulative_filled_quantity=*/0, /*fill_price=*/0.0);
  long_cancelled.status = aquila::core::OrderStatus::kCancelled;
  long_cancelled.place_request.reduce_only = true;
  StampOrderGroup(*unchanged_long, &long_cancelled);
  EXPECT_EQ(state.ApplyTerminalOrder(long_cancelled, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedHold);
  const leadlag::ExecutionGroup* retriable_long =
      state.FindGroupById(long_group_id);
  ASSERT_NE(retriable_long, nullptr);
  EXPECT_EQ(retriable_long->stage, leadlag::ExecutionStage::kHold);
  EXPECT_DOUBLE_EQ(retriable_long->signed_position_quantity, 5.0);
  EXPECT_EQ(retriable_long->normal_close_retry_count, 1U);
}

TEST(LeadLagFeedbackStateTest,
     UnknownResultPreservesOtherGroupExitAndRecoversOnTerminal) {
  leadlag::ExecutionState state;
  ASSERT_TRUE(state.Init(/*parallel=*/2));
  const leadlag::ExecutionGroup* unknown_group =
      state.StartOpenOrder(/*local_order_id=*/91);
  leadlag::ExecutionGroup* held_group =
      state.AddHoldGroup(/*signed_position_quantity=*/-3,
                         /*trailing_price=*/150.0);
  ASSERT_NE(unknown_group, nullptr);
  ASSERT_NE(held_group, nullptr);
  const std::uint64_t unknown_group_id = unknown_group->group_id;
  const std::uint64_t held_group_id = held_group->group_id;
  aquila::core::StrategyOrder unknown_order =
      Order(/*local_order_id=*/91, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/0, /*fill_price=*/0.0);
  unknown_order.is_finished = false;
  StampOrderGroup(*unknown_group, &unknown_order);

  EXPECT_TRUE(state.MarkUnknownResult(unknown_order));
  EXPECT_TRUE(state.degraded());
  EXPECT_TRUE(state.needs_reconcile());
  EXPECT_TRUE(state.new_entries_paused());
  EXPECT_TRUE(state.FindGroupById(held_group_id)->can_submit_exit());
  EXPECT_TRUE(state.StartCloseOrder(*held_group, /*local_order_id=*/92,
                                    leadlag::CloseOrderKind::kNormal));

  aquila::core::StrategyOrder resolved =
      Order(/*local_order_id=*/91, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/0, /*fill_price=*/0.0);
  resolved.status = aquila::core::OrderStatus::kCancelled;
  StampOrderGroup(*state.FindGroupById(unknown_group_id), &resolved);
  EXPECT_EQ(state.ApplyTerminalOrder(resolved, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedDeleted);
  EXPECT_EQ(state.FindGroupById(unknown_group_id), nullptr);
  EXPECT_FALSE(state.degraded());
  EXPECT_FALSE(state.needs_reconcile());
  EXPECT_TRUE(state.ConsumeUnknownResultAutoRecovered());
  const leadlag::ExecutionGroup* closing_group =
      state.FindGroupById(held_group_id);
  ASSERT_NE(closing_group, nullptr);
  EXPECT_EQ(closing_group->stage, leadlag::ExecutionStage::kClose);
  EXPECT_DOUBLE_EQ(closing_group->signed_position_quantity, -3.0);
  EXPECT_EQ(closing_group->pending_local_order_ids[0], 92U);
}

TEST(LeadLagFeedbackStateTest,
     ContinuityLostPreservesOpenAndClosingGroupState) {
  leadlag::ExecutionState state;
  ASSERT_TRUE(state.Init(/*parallel=*/2));
  const leadlag::ExecutionGroup* open_group =
      state.StartOpenOrder(/*local_order_id=*/101);
  leadlag::ExecutionGroup* closing_group =
      state.AddHoldGroup(/*signed_position_quantity=*/2,
                         /*trailing_price=*/120.0);
  ASSERT_NE(open_group, nullptr);
  ASSERT_NE(closing_group, nullptr);
  const std::uint64_t open_group_id = open_group->group_id;
  const std::uint64_t closing_group_id = closing_group->group_id;
  ASSERT_TRUE(state.StartCloseOrder(*closing_group, /*local_order_id=*/102,
                                    leadlag::CloseOrderKind::kStoploss));

  state.OnFeedbackContinuityLost(aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kContinuityLost,
  });

  EXPECT_TRUE(state.degraded());
  EXPECT_TRUE(state.needs_reconcile());
  ASSERT_EQ(state.active_group_count(), 2U);
  const leadlag::ExecutionGroup* preserved_open =
      state.FindGroupById(open_group_id);
  const leadlag::ExecutionGroup* preserved_close =
      state.FindGroupById(closing_group_id);
  ASSERT_NE(preserved_open, nullptr);
  ASSERT_NE(preserved_close, nullptr);
  EXPECT_EQ(preserved_open->stage, leadlag::ExecutionStage::kOpen);
  EXPECT_EQ(preserved_open->pending_local_order_ids[0], 101U);
  EXPECT_EQ(preserved_close->stage, leadlag::ExecutionStage::kClose);
  EXPECT_EQ(preserved_close->pending_local_order_ids[0], 102U);
  EXPECT_DOUBLE_EQ(preserved_close->signed_position_quantity, 2.0);
  EXPECT_EQ(preserved_close->close_order_kind,
            leadlag::CloseOrderKind::kStoploss);
}

TEST(LeadLagFeedbackStateTest,
     LongAndShortGroupsCloseIndependentlyInReverseOrder) {
  leadlag::ExecutionState state;
  ASSERT_TRUE(state.Init(/*parallel=*/2));
  const leadlag::ExecutionGroup* long_open =
      state.StartOpenOrder(/*local_order_id=*/111);
  const leadlag::ExecutionGroup* short_open =
      state.StartOpenOrder(/*local_order_id=*/112);
  ASSERT_NE(long_open, nullptr);
  ASSERT_NE(short_open, nullptr);
  const std::uint64_t long_group_id = long_open->group_id;
  const std::uint64_t short_group_id = short_open->group_id;
  aquila::core::StrategyOrder long_filled =
      Order(/*local_order_id=*/111, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/2, /*fill_price=*/100.0);
  aquila::core::StrategyOrder short_filled =
      Order(/*local_order_id=*/112, aquila::OrderSide::kSell,
            /*cumulative_filled_quantity=*/3, /*fill_price=*/200.0);
  StampOrderGroup(*long_open, &long_filled);
  StampOrderGroup(*short_open, &short_filled);
  EXPECT_EQ(state.ApplyTerminalOrder(short_filled, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedHold);
  EXPECT_EQ(state.ApplyTerminalOrder(long_filled, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedHold);
  EXPECT_DOUBLE_EQ(state.FindGroupById(long_group_id)->signed_position_quantity,
                   2.0);
  EXPECT_DOUBLE_EQ(
      state.FindGroupById(short_group_id)->signed_position_quantity, -3.0);

  leadlag::ExecutionGroup* long_hold = state.FindGroupById(long_group_id);
  leadlag::ExecutionGroup* short_hold = state.FindGroupById(short_group_id);
  ASSERT_TRUE(state.StartCloseOrder(*long_hold, /*local_order_id=*/113));
  ASSERT_TRUE(state.StartCloseOrder(*short_hold, /*local_order_id=*/114));
  aquila::core::StrategyOrder short_closed =
      Order(/*local_order_id=*/114, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/3, /*fill_price=*/199.0);
  aquila::core::StrategyOrder long_closed =
      Order(/*local_order_id=*/113, aquila::OrderSide::kSell,
            /*cumulative_filled_quantity=*/2, /*fill_price=*/101.0);
  short_closed.place_request.reduce_only = true;
  long_closed.place_request.reduce_only = true;
  StampOrderGroup(*short_hold, &short_closed);
  StampOrderGroup(*long_hold, &long_closed);
  EXPECT_EQ(state.ApplyTerminalOrder(short_closed, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedDeleted);
  EXPECT_NE(state.FindGroupById(long_group_id), nullptr);
  EXPECT_EQ(state.ApplyTerminalOrder(long_closed, Instrument()),
            leadlag::ExecutionApplyResult::kAppliedDeleted);
  EXPECT_EQ(state.active_group_count(), 0U);
}

TEST(LeadLagFeedbackStateTest, FeedbackContinuityLostPausesNewOpens) {
  const leadlag::PairConfig pair = PairConfigForFeedback();
  leadlag::ExecutionState state;
  state.Init(pair.execute.parallel);
  state.OnFeedbackContinuityLost(aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kContinuityLost,
  });

  const leadlag::SignalDecision decision = leadlag::SignalEngine::OnLeadTick(
      pair, state, OpenLongMarketForFeedback(), ThresholdForFeedback(),
      leadlag::AlignmentSnapshot{
          .drift_ready = true,
          .drift_deviation = 0.0,
      });

  EXPECT_TRUE(state.degraded());
  EXPECT_TRUE(state.needs_reconcile());
  EXPECT_FALSE(decision.triggered);
  EXPECT_EQ(decision.reject_reason, leadlag::SignalRejectReason::kDegraded);
}

}  // namespace
