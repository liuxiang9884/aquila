#include <cstdint>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/cost_model.h"
#include "strategy/lead_lag/execution_state.h"
#include "strategy/lead_lag/recorders.h"
#include "strategy/lead_lag/signal.h"
#include "strategy/lead_lag/threshold.h"
#include "strategy/lead_lag/types.h"

namespace {

namespace leadlag = aquila::strategy::leadlag;

leadlag::QuoteSnapshot Quote(std::int64_t event_ns, double bid_price,
                             double ask_price) {
  return leadlag::QuoteSnapshot{
      .event_ns = event_ns,
      .bid_price = bid_price,
      .ask_price = ask_price,
  };
}

leadlag::PairConfig PairConfigForSignal() {
  leadlag::PairConfig pair;
  pair.symbol = "BTC_USDT";
  pair.symbol_id = 7;
  pair.lag_exchange = aquila::Exchange::kGate;
  pair.lag_taker_fee = 0.0001;
  pair.trigger = leadlag::TriggerConfig{
      .lead = 0.02,
      .close = 0.005,
      .lag_part = 0.5,
      .target_profit_rate = 0.001,
      .drift_limit = 0.02,
  };
  pair.execute = leadlag::ExecuteConfig{
      .open_notional = 100.0,
      .trailing_stop = 0.05,
      .max_entry_spread = 0.02,
      .parallel = 1,
  };
  pair.lag_instrument = leadlag::InstrumentMetadata{
      .exchange = aquila::Exchange::kGate,
      .exchange_symbol = "BTC_USDT",
  };
  return pair;
}

leadlag::ThresholdSnapshot ThresholdForSignal() {
  return leadlag::ThresholdSnapshot{
      .initialized = true,
      .up_entry = 0.02,
      .down_entry = -0.02,
      .up_exit = 0.005,
      .down_exit = -0.005,
  };
}

leadlag::SignalMarket OpenLongMarket() {
  return leadlag::SignalMarket{
      .lead = Quote(/*event_ns=*/10, /*bid_price=*/104.0,
                    /*ask_price=*/105.0),
      .lag = Quote(/*event_ns=*/10, /*bid_price=*/101.5,
                   /*ask_price=*/102.0),
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
              .lead_noise = 0.0,
              .lag_noise = 0.0,
              .lag_spread_mean = 0.4,
          },
  };
}

TEST(LeadLagSignalTest, CostModelKeepsEmbeddedFrictionOutOfRequiredEdge) {
  const leadlag::EntryCostBreakdown cost{
      .fee = 0.02,
      .spread = 0.03,
      .lag_spread_buffer = 0.04,
      .lead_noise = 0.05,
      .lag_noise = 0.06,
  };

  EXPECT_DOUBLE_EQ(cost.RequiredEdge(), 0.13);
  EXPECT_DOUBLE_EQ(cost.EmbeddedPriceFriction(), 0.07);
  EXPECT_DOUBLE_EQ(cost.RequiredEdgeWithTargetProfit(0.07), 0.20);
}

TEST(LeadLagSignalTest, OpenLongPassesAllGates) {
  const leadlag::SignalDecision decision = leadlag::SignalEngine::TryOpenLong(
      PairConfigForSignal(), OpenLongMarket(), ThresholdForSignal());

  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kOpenLong);
  EXPECT_EQ(decision.intent.side, aquila::OrderSide::kBuy);
  EXPECT_FALSE(decision.intent.reduce_only);
  EXPECT_DOUBLE_EQ(decision.intent.price, 102.0);
}

TEST(LeadLagSignalTest, OpenLongRejectsEachGate) {
  const leadlag::PairConfig pair = PairConfigForSignal();
  const leadlag::ThresholdSnapshot threshold = ThresholdForSignal();

  leadlag::SignalMarket price_diff = OpenLongMarket();
  price_diff.lag.ask_price = 105.0;
  EXPECT_EQ(leadlag::SignalEngine::TryOpenLong(pair, price_diff, threshold)
                .reject_reason,
            leadlag::SignalRejectReason::kPriceDiff);

  leadlag::SignalMarket threshold_block = OpenLongMarket();
  threshold_block.lead.bid_price = 101.5;
  threshold_block.lag.ask_price = 101.0;
  EXPECT_EQ(leadlag::SignalEngine::TryOpenLong(pair, threshold_block, threshold)
                .reject_reason,
            leadlag::SignalRejectReason::kThreshold);

  leadlag::SignalMarket lag_part = OpenLongMarket();
  lag_part.lag.bid_price = 103.5;
  EXPECT_EQ(leadlag::SignalEngine::TryOpenLong(pair, lag_part, threshold)
                .reject_reason,
            leadlag::SignalRejectReason::kLagPart);

  leadlag::PairConfig target_cost = pair;
  target_cost.trigger.target_profit_rate = 0.10;
  EXPECT_EQ(leadlag::SignalEngine::TryOpenLong(target_cost, OpenLongMarket(),
                                               threshold)
                .reject_reason,
            leadlag::SignalRejectReason::kEntryCost);

  leadlag::SignalMarket spread = OpenLongMarket();
  spread.lag.bid_price = 99.0;
  spread.recorder.lag_spread_mean = 3.0;
  EXPECT_EQ(
      leadlag::SignalEngine::TryOpenLong(pair, spread, threshold).reject_reason,
      leadlag::SignalRejectReason::kEntrySpread);
}

TEST(LeadLagSignalTest, OpenShortPassesMirrorGates) {
  const leadlag::SignalMarket market{
      .lead = Quote(/*event_ns=*/10, /*bid_price=*/96.0, /*ask_price=*/97.0),
      .lag = Quote(/*event_ns=*/10, /*bid_price=*/98.0, /*ask_price=*/98.5),
      .recorder =
          leadlag::RecorderSnapshot{
              .lead_extrema =
                  leadlag::BboExtremaSnapshot{
                      .valid = true,
                      .bid_min = 96.0,
                      .bid_max = 99.0,
                      .ask_min = 97.0,
                      .ask_max = 100.0,
                  },
              .lag_extrema =
                  leadlag::BboExtremaSnapshot{
                      .valid = true,
                      .bid_min = 98.0,
                      .bid_max = 98.5,
                      .ask_min = 98.5,
                      .ask_max = 99.0,
                  },
              .lag_spread_mean = 0.4,
          },
  };

  const leadlag::SignalDecision decision = leadlag::SignalEngine::TryOpenShort(
      PairConfigForSignal(), market, ThresholdForSignal());

  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kOpenShort);
  EXPECT_EQ(decision.intent.side, aquila::OrderSide::kSell);
  EXPECT_FALSE(decision.intent.reduce_only);
  EXPECT_DOUBLE_EQ(decision.intent.price, 98.0);
}

TEST(LeadLagSignalTest, OpenShortRejectsEachGate) {
  const leadlag::PairConfig pair = PairConfigForSignal();
  const leadlag::ThresholdSnapshot threshold = ThresholdForSignal();
  const leadlag::SignalMarket base{
      .lead = Quote(/*event_ns=*/10, /*bid_price=*/96.0, /*ask_price=*/97.0),
      .lag = Quote(/*event_ns=*/10, /*bid_price=*/98.0,
                   /*ask_price=*/98.5),
      .recorder =
          leadlag::RecorderSnapshot{
              .lead_extrema =
                  leadlag::BboExtremaSnapshot{
                      .valid = true,
                      .bid_min = 96.0,
                      .bid_max = 99.0,
                      .ask_min = 97.0,
                      .ask_max = 100.0,
                  },
              .lag_extrema =
                  leadlag::BboExtremaSnapshot{
                      .valid = true,
                      .bid_min = 98.0,
                      .bid_max = 98.5,
                      .ask_min = 98.5,
                      .ask_max = 99.0,
                  },
              .lag_spread_mean = 0.4,
          },
  };

  leadlag::SignalMarket price_diff = base;
  price_diff.lag.bid_price = 96.0;
  EXPECT_EQ(leadlag::SignalEngine::TryOpenShort(pair, price_diff, threshold)
                .reject_reason,
            leadlag::SignalRejectReason::kPriceDiff);

  leadlag::SignalMarket threshold_block = base;
  threshold_block.lead.ask_price = 98.5;
  threshold_block.lag.bid_price = 99.5;
  EXPECT_EQ(
      leadlag::SignalEngine::TryOpenShort(pair, threshold_block, threshold)
          .reject_reason,
      leadlag::SignalRejectReason::kThreshold);

  leadlag::SignalMarket lag_part = base;
  lag_part.lag.bid_price = 97.5;
  lag_part.lag.ask_price = 98.0;
  EXPECT_EQ(leadlag::SignalEngine::TryOpenShort(pair, lag_part, threshold)
                .reject_reason,
            leadlag::SignalRejectReason::kLagPart);

  leadlag::PairConfig target_cost = pair;
  target_cost.trigger.target_profit_rate = 0.10;
  EXPECT_EQ(leadlag::SignalEngine::TryOpenShort(target_cost, base, threshold)
                .reject_reason,
            leadlag::SignalRejectReason::kEntryCost);

  leadlag::SignalMarket spread = base;
  spread.lag.bid_price = 98.0;
  spread.lag.ask_price = 103.0;
  spread.recorder.lag_spread_mean = 5.0;
  EXPECT_EQ(leadlag::SignalEngine::TryOpenShort(pair, spread, threshold)
                .reject_reason,
            leadlag::SignalRejectReason::kEntrySpread);
}

TEST(LeadLagSignalTest, LeadTickClosesHoldBeforeOpeningNewGroup) {
  const leadlag::PairConfig pair = PairConfigForSignal();
  leadlag::ExecutionState execution;
  execution.Init(pair.execute.parallel);
  ASSERT_NE(execution.AddHoldGroup(/*signed_position_quantity=*/1,
                                   /*trailing_price=*/100.0),
            nullptr);

  leadlag::SignalMarket market = OpenLongMarket();
  market.lead.bid_price = 100.0;
  market.lag.bid_price = 101.0;

  const leadlag::SignalDecision decision = leadlag::SignalEngine::OnLeadTick(
      pair, execution, market, ThresholdForSignal(),
      leadlag::AlignmentSnapshot{
          .drift_ready = true,
          .drift_deviation = 0.0,
      });

  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kCloseLong);
  EXPECT_TRUE(decision.intent.reduce_only);
  EXPECT_EQ(execution.active_group_count(), 1U);
}

TEST(LeadLagSignalTest, LagTickRunsStoplossBeforeSignalClose) {
  const leadlag::PairConfig pair = PairConfigForSignal();
  leadlag::ExecutionState execution;
  execution.Init(pair.execute.parallel);
  ASSERT_NE(execution.AddHoldGroup(/*signed_position_quantity=*/1,
                                   /*trailing_price=*/110.0),
            nullptr);

  const leadlag::SignalMarket market{
      .lead = Quote(/*event_ns=*/10, /*bid_price=*/95.0, /*ask_price=*/96.0),
      .lag = Quote(/*event_ns=*/10, /*bid_price=*/98.0, /*ask_price=*/98.5),
      .recorder = OpenLongMarket().recorder,
  };

  const leadlag::SignalDecision decision = leadlag::SignalEngine::OnLagTick(
      pair, execution, market, ThresholdForSignal());

  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kStoplossLong);
  EXPECT_TRUE(decision.intent.reduce_only);
  EXPECT_NEAR(decision.intent.price, 97.51, 1e-12);
}

}  // namespace
