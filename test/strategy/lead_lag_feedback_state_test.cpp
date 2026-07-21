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
