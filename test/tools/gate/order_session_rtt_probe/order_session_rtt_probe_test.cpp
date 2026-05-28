#include <fmt/format.h>
#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "exchange/gate/trading/order_types.h"
#include "tools/gate/order_session_rtt_probe/candidate_ip_list.h"
#include "tools/gate/order_session_rtt_probe/config.h"
#include "tools/gate/order_session_rtt_probe/cycle_scheduler.h"
#include "tools/gate/order_session_rtt_probe/passive_order_builder.h"
#include "tools/gate/order_session_rtt_probe/run_plan.h"
#include "tools/gate/order_session_rtt_probe/session_state.h"

namespace aquila::tools::gate_order_session_rtt_probe {
namespace {

[[nodiscard]] ProbeConfigResult ParseMinimalProbeConfigWith(
    std::string_view toml_tail) {
  const std::string text = fmt::format(
      R"toml(
[probe]
name = "gate_order_session_rtt_probe"

[probe.inputs]
candidate_ip_file = "/home/liuxiang/tmp/candidate_ips_login.txt"

{}
)toml",
      toml_tail);
  const toml::parse_result parsed = toml::parse(text);
  return ParseProbeConfig(parsed);
}

TEST(GateOrderSessionRttProbeTest, ParsesSingleSessionProbeConfig) {
  const toml::parse_result parsed = toml::parse(R"toml(
[probe]
name = "gate_order_session_rtt_probe"
execute = false
run_id = "unit_run"

[probe.inputs]
order_session_config = "config/order_sessions/gate_order_session.toml"
data_reader_config = "config/data_readers/strategy_data_reader_requested_20260521.toml"
candidate_ip_file = "/home/liuxiang/tmp/candidate_ips_login.txt"

[probe.sessions]
active_session_count = 1
max_candidates = 1
enable_tcp_info = true
wait_login_timeout_ms = 10000
request_timeout_ms = 5000
worker_cpu_ids = []

[probe.sampling]
samples_per_ip = 1
cycle_cooldown_ms = 500
max_events_per_drain = 128
idle_policy = "spin"
coordinator_cpu = -1

[probe.order]
side = "buy"
passive_price_limit_fraction = 0.5
quantity_mode = "min_quantity"
reduce_only_close = true

[probe.feedback]
enabled = true
shm_config = "config/order_feedback/gate_order_feedback_shm.toml"
strategy_id = 7
force_claim = false
poll_budget = 64
terminal_timeout_ms = 5000

[probe.safety]
preflight_rest_check = true
run_end_rest_check = true
rest_timeout_ms = 8000
rest_poll_interval_ms = 500
rest_poll_timeout_ms = 10000
stop_on_continuity_lost = true
confirm_dedicated_account = true

[probe.output]
root_dir = "/home/liuxiang/tmp/gate_order_session_rtt_probe"
)toml");

  const ProbeConfigResult result = ParseProbeConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "gate_order_session_rtt_probe");
  EXPECT_FALSE(result.value.execute);
  EXPECT_EQ(result.value.run_id, "unit_run");
  EXPECT_EQ(result.value.inputs.order_session_config,
            "config/order_sessions/gate_order_session.toml");
  EXPECT_EQ(result.value.sessions.active_session_count, 1U);
  EXPECT_EQ(result.value.sessions.max_candidates, 1U);
  EXPECT_TRUE(result.value.sessions.enable_tcp_info);
  EXPECT_EQ(result.value.sampling.cycle_cooldown_ms, 500U);
  EXPECT_EQ(result.value.order.passive_price_limit_fraction, 0.5);
  EXPECT_TRUE(result.value.order.reduce_only_close);
  EXPECT_EQ(result.value.feedback.strategy_id, 7U);
  EXPECT_TRUE(result.value.safety.run_end_rest_check);
  EXPECT_EQ(result.value.output.root_dir,
            "/home/liuxiang/tmp/gate_order_session_rtt_probe");
}

TEST(GateOrderSessionRttProbeTest, RejectsZeroActiveSessionCount) {
  const toml::parse_result parsed = toml::parse(R"toml(
[probe]
name = "gate_order_session_rtt_probe"

[probe.inputs]
order_session_config = "config/order_sessions/gate_order_session.toml"
data_reader_config = "config/data_readers/strategy_data_reader_requested_20260521.toml"
candidate_ip_file = "/home/liuxiang/tmp/candidate_ips_login.txt"

[probe.sessions]
active_session_count = 0
)toml");

  const ProbeConfigResult result = ParseProbeConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("active_session_count"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, RejectsWrongTypedConfigValues) {
  const ProbeConfigResult result = ParseMinimalProbeConfigWith(R"toml(
[probe.sessions]
active_session_count = "1"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("active_session_count"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, ValidatesFeedbackStrategyIdLane) {
  ProbeConfigResult result = ParseMinimalProbeConfigWith(R"toml(
[probe.feedback]
strategy_id = 0
)toml");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.feedback.strategy_id, 0U);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.feedback]
strategy_id = 8
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("strategy_id"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, RejectsUnsafeOrderConfig) {
  ProbeConfigResult result = ParseMinimalProbeConfigWith(R"toml(
[probe.order]
passive_price_limit_fraction = 0.0
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("passive_price_limit_fraction"),
            std::string::npos);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.order]
reduce_only_close = false
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("reduce_only_close"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, RejectsInvalidCpuConfig) {
  ProbeConfigResult result = ParseMinimalProbeConfigWith(R"toml(
[probe.sampling]
coordinator_cpu = 9223372036854775807
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("coordinator_cpu"), std::string::npos);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.sessions]
worker_cpu_ids = [0, "bad"]
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("worker_cpu_ids"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, RejectsUnsupportedConnectionGeneration) {
  const ProbeConfigResult result = ParseMinimalProbeConfigWith(R"toml(
[probe.sampling]
cycles_per_connection_generation = 2
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("cycles_per_connection_generation"),
            std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, RejectsDisabledSafetyGuardrails) {
  ProbeConfigResult result = ParseMinimalProbeConfigWith(R"toml(
[probe.feedback]
enabled = false
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("feedback.enabled"), std::string::npos);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.safety]
preflight_rest_check = false
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("preflight_rest_check"), std::string::npos);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.safety]
run_end_rest_check = false
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("run_end_rest_check"), std::string::npos);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.safety]
stop_on_continuity_lost = false
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("stop_on_continuity_lost"), std::string::npos);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.safety]
confirm_dedicated_account = false
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("confirm_dedicated_account"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, RejectsWrongTypedSections) {
  const toml::parse_result parsed = toml::parse(R"toml(
[probe]
name = "gate_order_session_rtt_probe"
sessions = "bad"

[probe.inputs]
candidate_ip_file = "/home/liuxiang/tmp/candidate_ips_login.txt"
)toml");
  const ProbeConfigResult result = ParseProbeConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("probe.sessions"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest,
     LoadsCandidateIpsSkippingHeadersAndDeduping) {
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

TEST(GateOrderSessionRttProbeTest, RejectsInvalidCandidateIpLine) {
  const CandidateIpLoadResult result = LoadCandidateIpsFromText(
      "# schema=aquila.gate.order_session.candidate_ips.v1\n"
      R"json({"ip":"52.198.250.74"})json"
      "\n");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("invalid candidate ip"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, BuildsSingleSessionDryRunPlan) {
  ProbeConfig config;
  config.inputs.candidate_ip_file = "/home/liuxiang/tmp/candidate_ips.txt";
  config.sessions.active_session_count = 1;
  config.sessions.max_candidates = 1;
  config.sampling.samples_per_ip = 1;
  config.sampling.cycles_per_connection_generation = 1;

  const ProbeRunPlanResult result =
      BuildProbeRunPlanFromCandidateText(config,
                                         "# generated_by=test\n"
                                         "52.198.250.74\n"
                                         "52.199.212.24\n");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.candidate_ip_count, 1U);
  ASSERT_EQ(result.value.cycles.size(), 1U);
  EXPECT_EQ(result.value.cycles[0].cycle_index, 0U);
  EXPECT_EQ(result.value.cycles[0].connect_ips,
            (std::vector<std::string>{"52.198.250.74"}));
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
  EXPECT_EQ(scheduler.NextCycle().connect_ips,
            (std::vector<std::string>{"ip0", "ip1"}));
  EXPECT_EQ(scheduler.NextCycle().connect_ips,
            (std::vector<std::string>{"ip2", "ip3"}));
  EXPECT_EQ(scheduler.NextCycle().connect_ips,
            (std::vector<std::string>{"ip4"}));
  EXPECT_EQ(scheduler.NextCycle().connect_ips,
            (std::vector<std::string>{"ip0", "ip1"}));
}

TEST(GateOrderSessionRttProbeTest,
     DecidesSafetyCloseForGtcCancelRejectAndIocAck) {
  EXPECT_FALSE(ShouldSubmitGtcSafetyClose(
      SafetyCloseInput{.stage = ProbeStage::kGtcCancel,
                       .response_kind = gate::OrderResponseKind::kAck}));

  EXPECT_TRUE(ShouldSubmitGtcSafetyClose(SafetyCloseInput{
      .stage = ProbeStage::kGtcCancel,
      .response_kind = gate::OrderResponseKind::kCancelRejected}));

  EXPECT_TRUE(ShouldSubmitIocSafetyCloseAfterAck(
      SafetyCloseInput{.stage = ProbeStage::kIocPlace,
                       .response_kind = gate::OrderResponseKind::kAck}));

  EXPECT_EQ(ClassifySafetyCloseRejected(/*position_known_flat=*/true),
            SafetyCloseStatus::kRejectedFlatSafe);
}

}  // namespace
}  // namespace aquila::tools::gate_order_session_rtt_probe
