#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "core/trading/order_id.h"
#include "exchange/gate/trading/order_types.h"
#include "nova/utils/log.h"
#include "tools/gate/order_session_rtt_probe/candidate_ip_list.h"
#include "tools/gate/order_session_rtt_probe/config.h"
#include "tools/gate/order_session_rtt_probe/cycle_scheduler.h"
#include "tools/gate/order_session_rtt_probe/live_run_plan.h"
#include "tools/gate/order_session_rtt_probe/passive_order_builder.h"
#include "tools/gate/order_session_rtt_probe/run_plan.h"
#include "tools/gate/order_session_rtt_probe/sample_csv_writer.h"
#include "tools/gate/order_session_rtt_probe/sample_executor.h"
#include "tools/gate/order_session_rtt_probe/sample_flow.h"
#include "tools/gate/order_session_rtt_probe/sample_id_allocator.h"
#include "tools/gate/order_session_rtt_probe/session_config_builder.h"
#include "tools/gate/order_session_rtt_probe/session_state.h"

namespace aquila::tools::gate_order_session_rtt_probe {
namespace {

std::filesystem::path TestTmpDir() {
  const std::filesystem::path path{"/home/liuxiang/tmp"};
  std::filesystem::create_directories(path);
  return path;
}

void EnsureRttProbeLoggingStarted() {
  static const bool started = [] {
    nova::LogConfig config;
    config.set_console_sink_name("");
    config.set_file_sink_name(
        (TestTmpDir() / "aquila_order_session_rtt_probe_test.log").string());
    nova::InitializeLogging(config);
    return true;
  }();
  (void)started;
}

std::string ReadTextFileForTest(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

struct FakeProbeOrderSession {
  struct SentOrder {
    std::string action;
    ProbeWireOrder order;
    std::uint64_t request_sequence{0};
    std::int64_t send_local_ns{0};
  };

  gate::OrderSendResult PlaceOrder(const ProbeWireOrder& order) {
    return Record("place", order);
  }

  gate::OrderSendResult CancelOrder(const ProbeWireOrder& order) {
    return Record("cancel", order);
  }

  gate::OrderSendResult Record(std::string action,
                               const ProbeWireOrder& order) {
    const std::uint64_t sequence = next_request_sequence++;
    const std::int64_t send_ns = next_send_local_ns;
    next_send_local_ns += 1000;
    sent_orders.push_back(SentOrder{.action = std::move(action),
                                    .order = order,
                                    .request_sequence = sequence,
                                    .send_local_ns = send_ns});
    return gate::OrderSendResult{
        .status = gate::OrderSendStatus::kOk,
        .request_sequence = sequence,
        .send_local_ns = send_ns,
    };
  }

  std::uint64_t next_request_sequence{11};
  std::int64_t next_send_local_ns{1000};
  std::vector<SentOrder> sent_orders;
};

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

TEST(GateOrderSessionRttProbeTest, BuildsProbeSampleOrders) {
  const PassiveOrderBuildResult passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "75.0",
      .quantity_text = "0.1",
      .bbo_ticker_id = 42,
      .bbo_local_ns = 2000,
  };
  ProbeSampleLocalIds ids{
      .gtc_local_order_id = 0x0700000000000001ULL,
      .ioc_local_order_id = 0x0700000000000002ULL,
      .gtc_close_local_order_id = 0x0700000000000003ULL,
      .ioc_close_local_order_id = 0x0700000000000004ULL,
  };

  const ProbeWireOrder gtc = BuildGtcPlaceOrder(passive, ids);
  const ProbeWireOrder cancel = BuildGtcCancelOrder(gtc);
  const ProbeWireOrder ioc = BuildIocPlaceOrder(passive, ids);
  const ProbeWireOrder close = BuildIocCloseOrder(passive, ids);

  EXPECT_EQ(gtc.local_order_id, ids.gtc_local_order_id);
  EXPECT_EQ(gtc.symbol, "ZEC_USDT");
  EXPECT_EQ(gtc.side, OrderSide::kBuy);
  EXPECT_EQ(gtc.time_in_force, TimeInForce::kGoodTillCancel);
  EXPECT_EQ(gtc.price_text, "75.0");
  EXPECT_FALSE(gtc.reduce_only);

  EXPECT_EQ(cancel.local_order_id, ids.gtc_local_order_id);
  EXPECT_EQ(cancel.symbol, "ZEC_USDT");

  EXPECT_EQ(ioc.local_order_id, ids.ioc_local_order_id);
  EXPECT_EQ(ioc.time_in_force, TimeInForce::kImmediateOrCancel);
  EXPECT_EQ(ioc.price_text, "75.0");

  EXPECT_EQ(close.local_order_id, ids.ioc_close_local_order_id);
  EXPECT_EQ(close.side, OrderSide::kSell);
  EXPECT_EQ(close.time_in_force, TimeInForce::kImmediateOrCancel);
  EXPECT_EQ(close.price_text, "0");
  EXPECT_TRUE(close.reduce_only);
}

TEST(GateOrderSessionRttProbeTest, AdvancesSampleFlowOnAckResponses) {
  ProbeSampleFlow flow(ProbeSampleLocalIds{
      .gtc_local_order_id = 0x0700000000000001ULL,
      .ioc_local_order_id = 0x0700000000000002ULL,
      .gtc_close_local_order_id = 0x0700000000000003ULL,
      .ioc_close_local_order_id = 0x0700000000000004ULL,
  });

  EXPECT_EQ(flow.Start(), ProbeSampleAction::kSubmitGtcPlace);
  ASSERT_TRUE(flow.OnOrderSent(ProbeStage::kGtcPlace,
                               gate::OrderSendResult{
                                   .status = gate::OrderSendStatus::kOk,
                                   .request_sequence = 11,
                                   .send_local_ns = 1000,
                               })
                  .ok);
  EXPECT_EQ(flow.stats().gtc_place_status, ProbeStageStatus::kSent);

  ProbeSampleTransition transition = flow.OnOrderResponse(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kAck,
      .local_order_id = 0x0700000000000001ULL,
      .request_sequence = 11,
      .local_receive_ns = 1600,
  });
  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kSubmitGtcCancel);
  EXPECT_EQ(flow.stats().gtc_place_ack_receive_local_ns, 1600);
  EXPECT_EQ(flow.stats().gtc_place_ack_rtt_ns, 600);
  EXPECT_EQ(flow.stats().gtc_place_status, ProbeStageStatus::kAcked);

  ASSERT_TRUE(flow.OnOrderSent(ProbeStage::kGtcCancel,
                               gate::OrderSendResult{
                                   .status = gate::OrderSendStatus::kOk,
                                   .request_sequence = 12,
                                   .send_local_ns = 2000,
                               })
                  .ok);
  EXPECT_EQ(flow.stats().gtc_cancel_status, ProbeStageStatus::kSent);
  transition = flow.OnOrderResponse(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kAck,
      .local_order_id = 0x0700000000000001ULL,
      .request_sequence = 12,
      .local_receive_ns = 2900,
  });
  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kSubmitIocPlace);
  EXPECT_EQ(flow.stats().gtc_cancel_ack_receive_local_ns, 2900);
  EXPECT_EQ(flow.stats().gtc_cancel_ack_rtt_ns, 900);
  EXPECT_EQ(flow.stats().gtc_cancel_status, ProbeStageStatus::kAcked);

  ASSERT_TRUE(flow.OnOrderSent(ProbeStage::kIocPlace,
                               gate::OrderSendResult{
                                   .status = gate::OrderSendStatus::kOk,
                                   .request_sequence = 13,
                                   .send_local_ns = 3000,
                               })
                  .ok);
  EXPECT_EQ(flow.stats().ioc_place_status, ProbeStageStatus::kSent);
  transition = flow.OnOrderResponse(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kAck,
      .local_order_id = 0x0700000000000002ULL,
      .request_sequence = 13,
      .local_receive_ns = 3700,
  });
  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kSubmitIocClose);
  EXPECT_EQ(flow.stats().ioc_place_ack_receive_local_ns, 3700);
  EXPECT_EQ(flow.stats().ioc_place_ack_rtt_ns, 700);
  EXPECT_EQ(flow.stats().ioc_place_status, ProbeStageStatus::kAcked);
}

TEST(GateOrderSessionRttProbeTest, RejectsAckWithMismatchedLocalOrderId) {
  ProbeSampleFlow flow(ProbeSampleLocalIds{
      .gtc_local_order_id = 0x0700000000000001ULL,
      .ioc_local_order_id = 0x0700000000000002ULL,
      .gtc_close_local_order_id = 0x0700000000000003ULL,
      .ioc_close_local_order_id = 0x0700000000000004ULL,
  });

  ASSERT_EQ(flow.Start(), ProbeSampleAction::kSubmitGtcPlace);
  ASSERT_TRUE(flow.OnOrderSent(ProbeStage::kGtcPlace,
                               gate::OrderSendResult{
                                   .status = gate::OrderSendStatus::kOk,
                                   .request_sequence = 11,
                                   .send_local_ns = 1000,
                               })
                  .ok);

  const ProbeSampleTransition transition =
      flow.OnOrderResponse(gate::OrderResponse{
          .kind = gate::OrderResponseKind::kAck,
          .local_order_id = 0x0700000000000002ULL,
          .request_sequence = 11,
          .local_receive_ns = 1600,
      });

  EXPECT_FALSE(transition.ok);
  EXPECT_EQ(transition.action, ProbeSampleAction::kFail);
  EXPECT_NE(transition.error.find("local_order_id"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest,
     DispatchesSingleSampleOrdersFromAckResponses) {
  const PassiveOrderBuildResult gtc_passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "75.0",
      .quantity_text = "0.1",
      .bbo_ticker_id = 42,
      .bbo_local_ns = 2000,
  };
  const PassiveOrderBuildResult ioc_passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "74.9",
      .quantity_text = "0.1",
      .bbo_ticker_id = 43,
      .bbo_local_ns = 3000,
  };
  auto executor_result = ProbeSampleExecutor::Create(
      gtc_passive, ioc_passive,
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      });
  ASSERT_TRUE(executor_result.ok) << executor_result.error;
  ProbeSampleExecutor& executor = *executor_result.value;
  FakeProbeOrderSession session;

  ProbeSampleTransition transition = executor.Start(session);
  ASSERT_TRUE(transition.ok) << transition.error;
  ASSERT_EQ(session.sent_orders.size(), 1U);
  EXPECT_EQ(session.sent_orders[0].action, "place");
  EXPECT_EQ(session.sent_orders[0].order.local_order_id, 0x0700000000000001ULL);
  EXPECT_EQ(session.sent_orders[0].order.time_in_force,
            TimeInForce::kGoodTillCancel);

  transition = executor.OnOrderResponse(
      session,
      gate::OrderResponse{
          .kind = gate::OrderResponseKind::kAck,
          .local_order_id = 0x0700000000000001ULL,
          .request_sequence = session.sent_orders[0].request_sequence,
          .local_receive_ns = session.sent_orders[0].send_local_ns + 600,
      });
  ASSERT_TRUE(transition.ok) << transition.error;
  ASSERT_EQ(session.sent_orders.size(), 2U);
  EXPECT_EQ(session.sent_orders[1].action, "cancel");
  EXPECT_EQ(session.sent_orders[1].order.local_order_id, 0x0700000000000001ULL);
  EXPECT_EQ(executor.stats().gtc_place_ack_rtt_ns, 600);

  transition = executor.OnOrderResponse(
      session,
      gate::OrderResponse{
          .kind = gate::OrderResponseKind::kAck,
          .local_order_id = 0x0700000000000001ULL,
          .request_sequence = session.sent_orders[1].request_sequence,
          .local_receive_ns = session.sent_orders[1].send_local_ns + 700,
      });
  ASSERT_TRUE(transition.ok) << transition.error;
  ASSERT_EQ(session.sent_orders.size(), 3U);
  EXPECT_EQ(session.sent_orders[2].action, "place");
  EXPECT_EQ(session.sent_orders[2].order.local_order_id, 0x0700000000000002ULL);
  EXPECT_EQ(session.sent_orders[2].order.time_in_force,
            TimeInForce::kImmediateOrCancel);
  EXPECT_EQ(session.sent_orders[2].order.price_text, "74.9");
  EXPECT_EQ(executor.stats().gtc_cancel_ack_rtt_ns, 700);

  transition = executor.OnOrderResponse(
      session,
      gate::OrderResponse{
          .kind = gate::OrderResponseKind::kAck,
          .local_order_id = 0x0700000000000002ULL,
          .request_sequence = session.sent_orders[2].request_sequence,
          .local_receive_ns = session.sent_orders[2].send_local_ns + 800,
      });
  ASSERT_TRUE(transition.ok) << transition.error;
  ASSERT_EQ(session.sent_orders.size(), 4U);
  EXPECT_EQ(session.sent_orders[3].action, "place");
  EXPECT_EQ(session.sent_orders[3].order.local_order_id, 0x0700000000000004ULL);
  EXPECT_EQ(session.sent_orders[3].order.side, OrderSide::kSell);
  EXPECT_EQ(session.sent_orders[3].order.price_text, "0");
  EXPECT_TRUE(session.sent_orders[3].order.reduce_only);
  EXPECT_EQ(executor.stats().ioc_place_ack_rtt_ns, 800);
}

TEST(GateOrderSessionRttProbeTest,
     RejectsInvalidPassiveOrdersBeforeCreatingExecutor) {
  const PassiveOrderBuildResult invalid_gtc{
      .ok = false,
      .contract = "ZEC_USDT",
      .error = "missing BBO",
  };
  const PassiveOrderBuildResult ioc_passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "74.9",
      .quantity_text = "0.1",
      .bbo_ticker_id = 43,
      .bbo_local_ns = 3000,
  };

  const auto result = ProbeSampleExecutor::Create(
      invalid_gtc, ioc_passive,
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      });

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.value, nullptr);
  EXPECT_NE(result.error.find("gtc passive"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, DispatchesGtcSafetyCloseOnCancelRejected) {
  const PassiveOrderBuildResult gtc_passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "75.0",
      .quantity_text = "0.1",
      .bbo_ticker_id = 42,
      .bbo_local_ns = 2000,
  };
  const PassiveOrderBuildResult ioc_passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "74.9",
      .quantity_text = "0.1",
      .bbo_ticker_id = 43,
      .bbo_local_ns = 3000,
  };
  auto executor_result = ProbeSampleExecutor::Create(
      gtc_passive, ioc_passive,
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      });
  ASSERT_TRUE(executor_result.ok) << executor_result.error;
  ProbeSampleExecutor& executor = *executor_result.value;
  FakeProbeOrderSession session;

  ASSERT_TRUE(executor.Start(session).ok);
  ASSERT_EQ(session.sent_orders.size(), 1U);
  ASSERT_TRUE(
      executor
          .OnOrderResponse(
              session,
              gate::OrderResponse{
                  .kind = gate::OrderResponseKind::kAck,
                  .local_order_id = 0x0700000000000001ULL,
                  .request_sequence = session.sent_orders[0].request_sequence,
                  .local_receive_ns =
                      session.sent_orders[0].send_local_ns + 600,
              })
          .ok);
  ASSERT_EQ(session.sent_orders.size(), 2U);

  const ProbeSampleTransition transition = executor.OnOrderResponse(
      session,
      gate::OrderResponse{
          .kind = gate::OrderResponseKind::kCancelRejected,
          .local_order_id = 0x0700000000000001ULL,
          .request_sequence = session.sent_orders[1].request_sequence,
          .local_receive_ns = session.sent_orders[1].send_local_ns + 700,
      });

  ASSERT_TRUE(transition.ok) << transition.error;
  ASSERT_EQ(session.sent_orders.size(), 3U);
  EXPECT_EQ(session.sent_orders[2].action, "place");
  EXPECT_EQ(session.sent_orders[2].order.local_order_id, 0x0700000000000003ULL);
  EXPECT_EQ(session.sent_orders[2].order.side, OrderSide::kSell);
  EXPECT_TRUE(session.sent_orders[2].order.reduce_only);
  EXPECT_EQ(executor.stats().gtc_cancel_status, ProbeStageStatus::kRejected);
}

TEST(GateOrderSessionRttProbeTest,
     DispatchesGtcSafetyCloseOnCancelRejectedAfterCancelAck) {
  const PassiveOrderBuildResult gtc_passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "75.0",
      .quantity_text = "0.1",
      .bbo_ticker_id = 42,
      .bbo_local_ns = 2000,
  };
  const PassiveOrderBuildResult ioc_passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "74.9",
      .quantity_text = "0.1",
      .bbo_ticker_id = 43,
      .bbo_local_ns = 3000,
  };
  auto executor_result = ProbeSampleExecutor::Create(
      gtc_passive, ioc_passive,
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      });
  ASSERT_TRUE(executor_result.ok) << executor_result.error;
  ProbeSampleExecutor& executor = *executor_result.value;
  FakeProbeOrderSession session;

  ASSERT_TRUE(executor.Start(session).ok);
  ASSERT_TRUE(
      executor
          .OnOrderResponse(
              session,
              gate::OrderResponse{
                  .kind = gate::OrderResponseKind::kAck,
                  .local_order_id = 0x0700000000000001ULL,
                  .request_sequence = session.sent_orders[0].request_sequence,
                  .local_receive_ns =
                      session.sent_orders[0].send_local_ns + 600,
              })
          .ok);
  ASSERT_EQ(session.sent_orders.size(), 2U);
  ASSERT_TRUE(
      executor
          .OnOrderResponse(
              session,
              gate::OrderResponse{
                  .kind = gate::OrderResponseKind::kAck,
                  .local_order_id = 0x0700000000000001ULL,
                  .request_sequence = session.sent_orders[1].request_sequence,
                  .local_receive_ns =
                      session.sent_orders[1].send_local_ns + 700,
              })
          .ok);
  ASSERT_EQ(session.sent_orders.size(), 3U);
  EXPECT_EQ(session.sent_orders[2].order.local_order_id, 0x0700000000000002ULL);

  const ProbeSampleTransition transition = executor.OnOrderResponse(
      session,
      gate::OrderResponse{
          .kind = gate::OrderResponseKind::kCancelRejected,
          .local_order_id = 0x0700000000000001ULL,
          .request_sequence = session.sent_orders[1].request_sequence,
          .local_receive_ns = session.sent_orders[1].send_local_ns + 900,
      });

  ASSERT_TRUE(transition.ok) << transition.error;
  ASSERT_EQ(session.sent_orders.size(), 4U);
  EXPECT_EQ(session.sent_orders[3].action, "place");
  EXPECT_EQ(session.sent_orders[3].order.local_order_id, 0x0700000000000003ULL);
  EXPECT_EQ(session.sent_orders[3].order.side, OrderSide::kSell);
  EXPECT_TRUE(session.sent_orders[3].order.reduce_only);
  EXPECT_EQ(executor.stats().gtc_cancel_ack_rtt_ns, 700);
  EXPECT_EQ(executor.stats().gtc_cancel_status, ProbeStageStatus::kRejected);
}

TEST(GateOrderSessionRttProbeTest,
     FailsSampleWhenSafetyCloseRejectedWithoutFlatProof) {
  const PassiveOrderBuildResult gtc_passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "75.0",
      .quantity_text = "0.1",
      .bbo_ticker_id = 42,
      .bbo_local_ns = 2000,
  };
  const PassiveOrderBuildResult ioc_passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "74.9",
      .quantity_text = "0.1",
      .bbo_ticker_id = 43,
      .bbo_local_ns = 3000,
  };
  auto executor_result = ProbeSampleExecutor::Create(
      gtc_passive, ioc_passive,
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      });
  ASSERT_TRUE(executor_result.ok) << executor_result.error;
  ProbeSampleExecutor& executor = *executor_result.value;
  FakeProbeOrderSession session;

  ASSERT_TRUE(executor.Start(session).ok);
  ASSERT_TRUE(
      executor
          .OnOrderResponse(
              session,
              gate::OrderResponse{
                  .kind = gate::OrderResponseKind::kAck,
                  .local_order_id = 0x0700000000000001ULL,
                  .request_sequence = session.sent_orders[0].request_sequence,
                  .local_receive_ns =
                      session.sent_orders[0].send_local_ns + 600,
              })
          .ok);
  ASSERT_TRUE(
      executor
          .OnOrderResponse(
              session,
              gate::OrderResponse{
                  .kind = gate::OrderResponseKind::kCancelRejected,
                  .local_order_id = 0x0700000000000001ULL,
                  .request_sequence = session.sent_orders[1].request_sequence,
                  .local_receive_ns =
                      session.sent_orders[1].send_local_ns + 700,
              })
          .ok);
  ASSERT_EQ(session.sent_orders.size(), 3U);

  const ProbeSampleTransition transition = executor.OnOrderResponse(
      session,
      gate::OrderResponse{
          .kind = gate::OrderResponseKind::kRejected,
          .local_order_id = 0x0700000000000003ULL,
          .request_sequence = session.sent_orders[2].request_sequence,
          .local_receive_ns = session.sent_orders[2].send_local_ns + 800,
      });

  EXPECT_FALSE(transition.ok);
  EXPECT_EQ(transition.action, ProbeSampleAction::kFail);
  EXPECT_NE(transition.error.find("safety close rejected"), std::string::npos);
  EXPECT_EQ(executor.stats().gtc_close_status, ProbeStageStatus::kRejected);
}

TEST(GateOrderSessionRttProbeTest,
     AllocatesSampleLocalOrderIdsInFeedbackStrategyLane) {
  ProbeSampleIdAllocator allocator(/*strategy_id=*/7);

  const ProbeSampleLocalIds first = allocator.Next();
  EXPECT_EQ(LocalOrderIdCodec::StrategyId(first.gtc_local_order_id), 7U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(first.gtc_local_order_id), 1U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(first.ioc_local_order_id), 2U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(first.gtc_close_local_order_id),
            3U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(first.ioc_close_local_order_id),
            4U);

  const ProbeSampleLocalIds second = allocator.Next();
  EXPECT_EQ(LocalOrderIdCodec::StrategyId(second.gtc_local_order_id), 7U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(second.gtc_local_order_id), 5U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(second.ioc_local_order_id), 6U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(second.gtc_close_local_order_id),
            7U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(second.ioc_close_local_order_id),
            8U);
}

TEST(GateOrderSessionRttProbeTest, BuildsPinnedOrderSessionConfig) {
  gate::OrderSessionConfig base;
  base.name = "base_gate_order_session";
  base.connection.host = "fx-ws.gateio.ws";
  base.connection.connect_ip = "";
  base.connection.port = "443";
  base.connection.target = "/v4/ws/usdt";
  base.connection.runtime_policy.io_cpu_id = 3;
  base.enable_tcp_info_diagnostics = false;

  const gate::OrderSessionConfig pinned = BuildPinnedOrderSessionConfig(
      base, PinnedOrderSessionOptions{
                .connect_ip = "52.198.250.74",
                .worker_cpu_id = 6,
                .enable_tcp_info_diagnostics = true,
            });

  EXPECT_EQ(pinned.name, "base_gate_order_session");
  EXPECT_EQ(pinned.connection.host, "fx-ws.gateio.ws");
  EXPECT_EQ(pinned.connection.connect_ip, "52.198.250.74");
  EXPECT_EQ(pinned.connection.port, "443");
  EXPECT_EQ(pinned.connection.target, "/v4/ws/usdt");
  EXPECT_EQ(pinned.connection.runtime_policy.io_cpu_id, 6);
  EXPECT_TRUE(pinned.enable_tcp_info_diagnostics);
}

TEST(GateOrderSessionRttProbeTest, BuildsSingleSessionLiveRunPlan) {
  ProbeConfig config;
  config.run_id = "run_1";
  config.output.root_dir = "/home/liuxiang/tmp/gate_order_session_rtt_probe";
  config.sessions.active_session_count = 1;
  config.sessions.enable_tcp_info = true;
  config.sessions.worker_cpu_ids = {6};

  const ProbeRunPlan run_plan{
      .candidate_ip_count = 1,
      .duplicate_candidate_ip_count = 0,
      .cycles =
          {
              ProbeCycle{
                  .cycle_index = 0,
                  .group_index = 0,
                  .connect_ips = {"52.198.250.74"},
              },
              ProbeCycle{
                  .cycle_index = 1,
                  .group_index = 0,
                  .connect_ips = {"52.198.250.74"},
              },
          },
  };
  gate::OrderSessionConfig base;
  base.connection.host = "fx-ws.gateio.ws";
  base.connection.connect_ip = "";
  base.connection.port = "443";
  base.connection.target = "/v4/ws/usdt";
  base.connection.runtime_policy.io_cpu_id = 3;

  const SingleSessionLiveRunPlanResult result =
      BuildSingleSessionLiveRunPlan(config, run_plan, base);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.connect_ip, "52.198.250.74");
  EXPECT_EQ(result.value.sample_count, 2U);
  EXPECT_EQ(result.value.order_session_config.connection.host,
            "fx-ws.gateio.ws");
  EXPECT_EQ(result.value.order_session_config.connection.connect_ip,
            "52.198.250.74");
  EXPECT_EQ(
      result.value.order_session_config.connection.runtime_policy.io_cpu_id, 6);
  EXPECT_TRUE(result.value.order_session_config.enable_tcp_info_diagnostics);
  EXPECT_EQ(result.value.paths.run_dir,
            "/home/liuxiang/tmp/gate_order_session_rtt_probe/run_1");
  EXPECT_EQ(result.value.paths.sample_csv_path,
            "/home/liuxiang/tmp/gate_order_session_rtt_probe/run_1/"
            "order_session_rtt_samples.csv");
  EXPECT_EQ(result.value.paths.rest_guard_csv_path,
            "/home/liuxiang/tmp/gate_order_session_rtt_probe/run_1/"
            "order_session_rtt_rest_guard.csv");
  EXPECT_EQ(result.value.paths.raw_rest_dir,
            "/home/liuxiang/tmp/gate_order_session_rtt_probe/run_1/raw_rest");
}

TEST(GateOrderSessionRttProbeTest, RejectsMultiSessionLiveRunPlan) {
  ProbeConfig config;
  config.run_id = "run_1";
  config.sessions.active_session_count = 2;
  const ProbeRunPlan run_plan{
      .candidate_ip_count = 2,
      .cycles =
          {
              ProbeCycle{
                  .cycle_index = 0,
                  .group_index = 0,
                  .connect_ips = {"52.198.250.74", "52.199.212.24"},
              },
          },
  };

  const SingleSessionLiveRunPlanResult result = BuildSingleSessionLiveRunPlan(
      config, run_plan, gate::OrderSessionConfig{});

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("single-session"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, WritesSampleCsvRowsThroughQuillCsvWriter) {
  EnsureRttProbeLoggingStarted();
  const std::filesystem::path output_path =
      TestTmpDir() / "aquila_order_session_rtt_probe_samples_test.csv";
  std::filesystem::remove(output_path);

  SampleCsvWriter writer;
  std::string error;
  ASSERT_TRUE(writer.Open(output_path, &error)) << error;
  writer.Write(ProbeSampleCsvRow{
      .run_id = "run_1",
      .connect_ip = "52.198.250.74",
      .order_session_id = 7,
      .connection_generation = 0,
      .round_index = 2,
      .sample_index = 3,
      .contract = "ZEC_USDT",
      .quantity_text = "0.1",
      .sample_start_ns = 1000,
      .sample_end_ns = 2000,
      .gtc_bbo_ticker_id = 42,
      .gtc_bbo_local_ns = 900,
      .gtc_price_text = "75.0",
      .ioc_bbo_ticker_id = 43,
      .ioc_bbo_local_ns = 1400,
      .ioc_price_text = "74.9",
      .gtc_place_ack_receive_local_ns = 1100,
      .gtc_place_ack_rtt_ns = 100,
      .gtc_cancel_ack_receive_local_ns = 1300,
      .gtc_cancel_ack_rtt_ns = 200,
      .ioc_place_ack_receive_local_ns = 1600,
      .ioc_place_ack_rtt_ns = 300,
      .gtc_close_submitted = false,
      .gtc_close_ack_receive_local_ns = 0,
      .gtc_close_ack_rtt_ns = -1,
      .gtc_close_status = "not_submitted",
      .ioc_close_submitted = true,
      .ioc_close_ack_receive_local_ns = 1700,
      .ioc_close_ack_rtt_ns = 100,
      .ioc_close_status = "rejected_flat_safe",
      .gtc_place_status = "acked",
      .gtc_cancel_status = "acked",
      .ioc_place_status = "acked",
      .unexpected_fill = false,
      .invalid_for_rtt_distribution = false,
      .invalid_reason = "",
  });
  writer.Close();

  EXPECT_EQ(
      ReadTextFileForTest(output_path),
      "run_id,connect_ip,order_session_id,connection_generation,round_index,"
      "sample_index,contract,quantity_text,sample_start_ns,sample_end_ns,"
      "gtc_bbo_ticker_id,gtc_bbo_local_ns,gtc_price_text,ioc_bbo_ticker_id,"
      "ioc_bbo_local_ns,ioc_price_text,gtc_place_ack_receive_local_ns,"
      "gtc_place_ack_rtt_ns,gtc_cancel_ack_receive_local_ns,"
      "gtc_cancel_ack_rtt_ns,ioc_place_ack_receive_local_ns,"
      "ioc_place_ack_rtt_ns,gtc_close_submitted,"
      "gtc_close_ack_receive_local_ns,gtc_close_ack_rtt_ns,gtc_close_status,"
      "ioc_close_submitted,ioc_close_ack_receive_local_ns,"
      "ioc_close_ack_rtt_ns,ioc_close_status,gtc_place_status,"
      "gtc_cancel_status,ioc_place_status,unexpected_fill,"
      "invalid_for_rtt_distribution,invalid_reason\n"
      "run_1,52.198.250.74,7,0,2,3,ZEC_USDT,0.1,1000,2000,42,900,"
      "75.0,43,1400,74.9,1100,100,1300,200,1600,300,false,0,-1,"
      "not_submitted,true,1700,100,rejected_flat_safe,acked,acked,acked,"
      "false,false,\n");
}

TEST(GateOrderSessionRttProbeTest, SampleCsvWriterCreatesParentDirectory) {
  EnsureRttProbeLoggingStarted();
  const std::filesystem::path output_path =
      TestTmpDir() / "rtt_probe_nested" / "samples" / "samples.csv";
  std::filesystem::remove_all(output_path.parent_path().parent_path());

  SampleCsvWriter writer;
  std::string error;

  ASSERT_TRUE(writer.Open(output_path, &error)) << error;
  writer.Close();

  EXPECT_TRUE(std::filesystem::exists(output_path));
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
