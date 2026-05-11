#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/strategy/order_manager.h"
#include "core/strategy/order_types.h"
#include "core/trading/order_feedback_event.h"
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

  SendResult PlaceOrder(aquila::strategy::StrategyOrder&) noexcept {
    return {};
  }

  SendResult CancelOrder(aquila::strategy::StrategyOrder&) noexcept {
    return {};
  }
};

leadlag::InstrumentMetadata Instrument() {
  return leadlag::InstrumentMetadata{
      .quantity_step = 1.0,
      .notional_multiplier = 1.0,
  };
}

aquila::strategy::StrategyOrder Order(std::uint64_t local_order_id,
                                      aquila::OrderSide side,
                                      std::int64_t cumulative_filled_quantity,
                                      double fill_price) {
  return aquila::strategy::StrategyOrder{
      .local_order_id = local_order_id,
      .side = side,
      .quantity = cumulative_filled_quantity,
      .status = aquila::strategy::OrderStatus::kFilled,
      .cumulative_filled_quantity = cumulative_filled_quantity,
      .cumulative_filled_value =
          static_cast<double>(cumulative_filled_quantity) * fill_price,
      .last_fill_price = fill_price,
      .is_finished = true,
  };
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
      .drift_limit = 0.02,
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
              .local_ns = 10,
              .bid_price = 104.0,
              .ask_price = 105.0,
          },
      .lag =
          leadlag::QuoteSnapshot{
              .local_ns = 10,
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
  aquila::strategy::OrderManager<FakeOrderSession> manager(session, 2, 1);

  const aquila::strategy::OrderPlaceResult placed =
      manager.PlaceLimitOrder(aquila::strategy::OrderCreateRequest{
          .symbol = std::string_view{"BTC_USDT"},
          .time_in_force = aquila::TimeInForce::kImmediateOrCancel,
          .quantity = 1,
          .price_text = std::string_view{"100.0"},
      });

  ASSERT_EQ(placed.status, aquila::strategy::OrderPlaceStatus::kOk);
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
  ASSERT_NE(state.StartOpenOrder(/*local_order_id=*/11), nullptr);

  const leadlag::ExecutionApplyResult result = state.ApplyTerminalOrder(
      Order(/*local_order_id=*/11, aquila::OrderSide::kBuy,
            /*cumulative_filled_quantity=*/3, /*fill_price=*/102.0),
      Instrument());

  EXPECT_EQ(result, leadlag::ExecutionApplyResult::kAppliedHold);
  ASSERT_EQ(state.active_group_count(), 1U);
  const leadlag::ExecutionGroup* group = state.FindGroupById(1);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->stage, leadlag::ExecutionStage::kHold);
  EXPECT_EQ(group->signed_position_quantity, 3);
  EXPECT_EQ(group->local_order_id, 0U);
  EXPECT_DOUBLE_EQ(group->trailing_price, 102.0);
}

TEST(LeadLagFeedbackStateTest, SubmitRejectedDeletesEmptyOpenGroup) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/2);
  ASSERT_NE(state.StartOpenOrder(/*local_order_id=*/21), nullptr);

  EXPECT_EQ(state.ApplySubmitRejected(/*local_order_id=*/21),
            leadlag::ExecutionApplyResult::kAppliedDeleted);
  EXPECT_EQ(state.active_group_count(), 0U);
  EXPECT_EQ(state.ApplySubmitRejected(/*local_order_id=*/0),
            leadlag::ExecutionApplyResult::kIgnoredUnknownOrder);
  EXPECT_EQ(state.ApplySubmitRejected(/*local_order_id=*/21),
            leadlag::ExecutionApplyResult::kIgnoredUnknownOrder);
}

TEST(LeadLagFeedbackStateTest, CloseTerminalFeedbackDeletesFlatGroup) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/1);
  leadlag::ExecutionGroup* group =
      state.AddHoldGroup(/*signed_position_quantity=*/3,
                         /*trailing_price=*/102.0);
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(state.StartCloseOrder(*group, /*local_order_id=*/12));

  const leadlag::ExecutionApplyResult result = state.ApplyTerminalOrder(
      Order(/*local_order_id=*/12, aquila::OrderSide::kSell,
            /*cumulative_filled_quantity=*/3, /*fill_price=*/101.0),
      Instrument());

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

  aquila::strategy::StrategyOrder rejected =
      Order(/*local_order_id=*/13, aquila::OrderSide::kSell,
            /*cumulative_filled_quantity=*/0, /*fill_price=*/0.0);
  rejected.status = aquila::strategy::OrderStatus::kRejected;

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

TEST(LeadLagFeedbackStateTest, FeedbackGapPausesNewOpens) {
  const leadlag::PairConfig pair = PairConfigForFeedback();
  leadlag::ExecutionState state;
  state.Init(pair.execute.parallel);
  state.OnFeedbackGap(aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kGap,
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
