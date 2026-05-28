#include "tools/gate/order_session_rtt_probe/candidate_ip_list.h"
#include "tools/gate/order_session_rtt_probe/cycle_scheduler.h"
#include "tools/gate/order_session_rtt_probe/passive_order_builder.h"
#include "tools/gate/order_session_rtt_probe/session_state.h"

#include <gtest/gtest.h>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::tools::gate_order_session_rtt_probe {
namespace {

TEST(GateOrderSessionRttProbeTest, LoadsCandidateIpsSkippingHeadersAndDeduping) {
  const CandidateIpLoadResult result = LoadCandidateIpsFromText(
      "# schema=aquila.gate.order_session.candidate_ips.v1\n"
      "# generated_at_ns=1\n"
      "52.198.250.74\n"
      "52.199.212.24\n"
      "52.198.250.74\n"
      "\n"
      "57.181.9.46\n",
      CandidateIpLoadOptions{.max_candidates = 2});

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.ips.size(), 2U);
  EXPECT_EQ(result.ips[0], "52.198.250.74");
  EXPECT_EQ(result.ips[1], "52.199.212.24");
  EXPECT_EQ(result.duplicate_count, 1U);
}

TEST(GateOrderSessionRttProbeTest, BuildsPassiveBuyUsingHalfPriceLimitDown) {
  const BookTicker ticker{
      .id = 42,
      .symbol_id = 7,
      .exchange = Exchange::kGate,
      .exchange_ns = 1000,
      .local_ns = 2000,
      .bid_price = 100.0,
      .bid_volume = 10.0,
      .ask_price = 101.0,
      .ask_volume = 11.0,
  };
  const config::InstrumentInfo instrument{
      .symbol_id = 7,
      .exchange = Exchange::kGate,
      .symbol = "ZEC_USDT",
      .exchange_symbol = "ZEC_USDT",
      .price_tick = 0.1,
      .price_decimal_places = 1,
      .quantity_step = 0.1,
      .quantity_decimal_places = 1,
      .min_quantity = 0.1,
      .price_limit_down = 0.5,
  };

  const PassiveOrderBuildResult result =
      BuildPassiveBuyOrder(ticker, instrument,
                           PassiveOrderOptions{
                               .passive_price_limit_fraction = 0.5,
                           });

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.contract, "ZEC_USDT");
  EXPECT_EQ(result.quantity_text, "0.1");
  EXPECT_EQ(result.price_text, "75.0");
  EXPECT_EQ(result.bbo_ticker_id, 42);
  EXPECT_EQ(result.bbo_local_ns, 2000);
}

TEST(GateOrderSessionRttProbeTest, RotatesCandidateGroupsByActiveSessionCount) {
  CycleScheduler scheduler(CycleSchedulerOptions{
      .candidate_ips = {"ip0", "ip1", "ip2", "ip3", "ip4"},
      .active_session_count = 2,
      .samples_per_ip = 2,
      .cycles_per_connection_generation = 1,
  });

  ASSERT_TRUE(scheduler.HasNextCycle());
  EXPECT_EQ(scheduler.NextCycle().connect_ips, (std::vector<std::string>{"ip0", "ip1"}));
  EXPECT_EQ(scheduler.NextCycle().connect_ips, (std::vector<std::string>{"ip2", "ip3"}));
  EXPECT_EQ(scheduler.NextCycle().connect_ips, (std::vector<std::string>{"ip4"}));
  EXPECT_EQ(scheduler.NextCycle().connect_ips, (std::vector<std::string>{"ip0", "ip1"}));
}

TEST(GateOrderSessionRttProbeTest, DecidesSafetyCloseForGtcCancelRejectAndIocAck) {
  EXPECT_FALSE(ShouldSubmitGtcSafetyClose(
      SafetyCloseInput{.stage = ProbeStage::kGtcCancel,
                       .response_kind = gate::OrderResponseKind::kAck}));

  EXPECT_TRUE(ShouldSubmitGtcSafetyClose(
      SafetyCloseInput{.stage = ProbeStage::kGtcCancel,
                       .response_kind =
                           gate::OrderResponseKind::kCancelRejected}));

  EXPECT_TRUE(ShouldSubmitIocSafetyCloseAfterAck(
      SafetyCloseInput{.stage = ProbeStage::kIocPlace,
                       .response_kind = gate::OrderResponseKind::kAck}));

  EXPECT_EQ(ClassifySafetyCloseRejected(/*position_known_flat=*/true),
            SafetyCloseStatus::kRejectedFlatSafe);
}

}  // namespace
}  // namespace aquila::tools::gate_order_session_rtt_probe
