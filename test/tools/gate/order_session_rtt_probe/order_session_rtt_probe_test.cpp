#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "core/trading/order_id.h"
#include "exchange/gate/trading/order_types.h"
#include "nova/utils/log.h"
#include "tools/gate/order_session_rtt_probe/config.h"
#include "tools/gate/order_session_rtt_probe/connection_plan.h"
#include "tools/gate/order_session_rtt_probe/cycle_scheduler.h"
#include "tools/gate/order_session_rtt_probe/live_run_plan.h"
#include "tools/gate/order_session_rtt_probe/local_feedback_queue.h"
#include "tools/gate/order_session_rtt_probe/passive_order_builder.h"
#include "tools/gate/order_session_rtt_probe/run_plan.h"
#include "tools/gate/order_session_rtt_probe/sample_csv_writer.h"
#include "tools/gate/order_session_rtt_probe/sample_executor.h"
#include "tools/gate/order_session_rtt_probe/sample_flow.h"
#include "tools/gate/order_session_rtt_probe/sample_id_allocator.h"
#include "tools/gate/order_session_rtt_probe/session_config_builder.h"
#include "tools/gate/order_session_rtt_probe/session_state.h"
#include "tools/gate/order_session_rtt_probe/session_watchdog.h"

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

ProbeConnectionConfig MakeProbeConnection(std::string name,
                                          std::string connect_ip,
                                          std::int32_t worker_cpu_id) {
  return ProbeConnectionConfig{
      .name = std::move(name),
      .group = "public",
      .host = "fx-ws.gateio.ws",
      .connect_ip = std::move(connect_ip),
      .port = "443",
      .enable_tls = true,
      .worker_cpu_id = worker_cpu_id,
  };
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

struct FakeWatchdogSession {
  bool Start() {
    started = true;
    while (!stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return start_result;
  }

  void Stop() {
    stop_requested.store(true);
  }

  std::atomic<bool> stop_requested{false};
  bool started{false};
  bool start_result{true};
};

[[nodiscard]] ProbeConfigResult ParseMinimalProbeConfigWith(
    std::string_view toml_tail) {
  const std::string text = fmt::format(
      R"toml(
[probe]
name = "gate_order_session_rtt_probe"

[probe.inputs]
connections_file = "/home/liuxiang/tmp/connections.csv"

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
connections_file = "/home/liuxiang/tmp/connections.csv"

[probe.sessions]
enable_tcp_info = true
wait_login_timeout_ms = 10000
request_timeout_ms = 5000

[probe.sessions.timestamping]
enabled = true
tx_software = true
tx_ack = true
rx_software = true
max_errqueue_events_per_drain = 16

[probe.sampling]
samples_per_session = 1
cycle_cooldown_ms = 500
order_session_interval_ms = 25
max_events_per_drain = 128
idle_policy = "spin"
coordinator_cpu = -1

[probe.order]
order_mode = "ioc+gtc"
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
  EXPECT_EQ(result.value.inputs.connections_file,
            "/home/liuxiang/tmp/connections.csv");
  EXPECT_TRUE(result.value.sessions.enable_tcp_info);
  EXPECT_TRUE(result.value.sessions.timestamping.enabled);
  EXPECT_TRUE(result.value.sessions.timestamping.tx_software);
  EXPECT_TRUE(result.value.sessions.timestamping.tx_ack);
  EXPECT_TRUE(result.value.sessions.timestamping.rx_software);
  EXPECT_EQ(result.value.sessions.timestamping.max_errqueue_events_per_drain,
            16U);
  EXPECT_EQ(result.value.sampling.samples_per_session, 1U);
  EXPECT_EQ(result.value.sampling.cycle_cooldown_ms, 500U);
  EXPECT_EQ(result.value.sampling.order_session_interval_ms, 25U);
  EXPECT_EQ(result.value.order.order_mode, ProbeOrderMode::kIocAndGtc);
  EXPECT_EQ(result.value.order.passive_price_limit_fraction, 0.5);
  EXPECT_TRUE(result.value.order.reduce_only_close);
  EXPECT_EQ(result.value.feedback.strategy_id, 7U);
  EXPECT_TRUE(result.value.safety.run_end_rest_check);
  EXPECT_EQ(result.value.output.root_dir,
            "/home/liuxiang/tmp/gate_order_session_rtt_probe");
}

TEST(GateOrderSessionRttProbeTest, RejectsZeroSamplesPerSession) {
  const toml::parse_result parsed = toml::parse(R"toml(
[probe]
name = "gate_order_session_rtt_probe"

[probe.inputs]
order_session_config = "config/order_sessions/gate_order_session.toml"
data_reader_config = "config/data_readers/strategy_data_reader_requested_20260521.toml"
connections_file = "/home/liuxiang/tmp/connections.csv"

[probe.sampling]
samples_per_session = 0
)toml");

  const ProbeConfigResult result = ParseProbeConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("samples_per_session"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, RejectsWrongTypedConfigValues) {
  const ProbeConfigResult result = ParseMinimalProbeConfigWith(R"toml(
[probe.sampling]
samples_per_session = "1"
)toml");

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("samples_per_session"), std::string::npos);
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
order_mode = "bad"
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("order_mode"), std::string::npos);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.order]
order_mode = 1
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("order_mode"), std::string::npos);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.order]
order_mode = "ioc"
)toml");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.order.order_mode, ProbeOrderMode::kIoc);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.order]
order_mode = "gtc"
)toml");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.order.order_mode, ProbeOrderMode::kGtc);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.order]
order_mode = "gtc+ioc"
)toml");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.order.order_mode, ProbeOrderMode::kIocAndGtc);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.sampling]
order_session_interval_ms = 0
)toml");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.sampling.order_session_interval_ms, 0U);

  result = ParseMinimalProbeConfigWith(R"toml(
[probe.sampling]
order_session_interval_ms = -1
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("order_session_interval_ms"), std::string::npos);

  result = ParseMinimalProbeConfigWith(R"toml(
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
  const ProbeConfigResult result = ParseMinimalProbeConfigWith(R"toml(
[probe.sampling]
coordinator_cpu = 9223372036854775807
)toml");
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("coordinator_cpu"), std::string::npos);
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
connections_file = "/home/liuxiang/tmp/connections.csv"
)toml");
  const ProbeConfigResult result = ParseProbeConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("probe.sessions"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest,
     LoadsConnectionCsvPreservingDuplicateConnectIps) {
  const std::filesystem::path path =
      TestTmpDir() / "aquila_rtt_probe_connections.csv";
  {
    std::ofstream output(path);
    output << "name,group,host,connect_ip,port,enable_tls,worker_cpu_id\n"
           << "private-0,private-10.0.1.154,fxws-private.gateapi.io,"
              "10.0.1.154,80,false,6\n"
           << "public-1,public-13.159.186.99,fx-ws.gateio.ws,"
              "13.159.186.99,443,true,7\n"
           << "private-2,private-10.0.1.154,fxws-private.gateapi.io,"
              "10.0.1.154,80,false,8\n";
  }

  const ProbeConnectionsCsvResult result = LoadProbeConnectionsCsvFile(path);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.connections.size(), 3U);
  EXPECT_EQ(result.connections[0].name, "private-0");
  EXPECT_EQ(result.connections[0].group, "private-10.0.1.154");
  EXPECT_EQ(result.connections[0].host, "fxws-private.gateapi.io");
  EXPECT_EQ(result.connections[0].connect_ip, "10.0.1.154");
  EXPECT_EQ(result.connections[0].port, "80");
  EXPECT_FALSE(result.connections[0].enable_tls);
  EXPECT_EQ(result.connections[0].worker_cpu_id, 6);
  EXPECT_EQ(result.connections[2].connect_ip, "10.0.1.154");
  EXPECT_EQ(result.connections[2].worker_cpu_id, 8);
}

TEST(GateOrderSessionRttProbeTest, RejectsInvalidConnectionCsvWorkerCpu) {
  const std::filesystem::path path =
      TestTmpDir() / "aquila_rtt_probe_bad_connections.csv";
  {
    std::ofstream output(path);
    output << "name,group,host,connect_ip,port,enable_tls,worker_cpu_id\n"
           << "private-0,private,fxws-private.gateapi.io,10.0.1.154,80,"
              "false,bad\n";
  }

  const ProbeConnectionsCsvResult result = LoadProbeConnectionsCsvFile(path);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("worker_cpu_id"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, ParsesConnectionsFileProbeConfig) {
  const toml::parse_result parsed = toml::parse(R"toml(
[probe]
name = "gate_order_session_rtt_probe"

[probe.inputs]
connections_file = "/home/liuxiang/tmp/connections.csv"

[probe.sessions]
enable_tcp_info = true

[probe.sampling]
samples_per_session = 25
)toml");

  const ProbeConfigResult result = ParseProbeConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.inputs.connections_file,
            "/home/liuxiang/tmp/connections.csv");
  EXPECT_EQ(result.value.sampling.samples_per_session, 25U);
}

TEST(GateOrderSessionRttProbeTest, BuildsSingleSessionDryRunPlan) {
  ProbeConfig config;
  config.inputs.connections_file = "/home/liuxiang/tmp/connections.csv";
  config.sampling.samples_per_session = 1;
  config.sampling.cycles_per_connection_generation = 1;

  const ProbeRunPlanResult result =
      BuildProbeRunPlan(config, {ProbeConnectionConfig{
                                    .name = "public-0",
                                    .group = "public",
                                    .host = "fx-ws.gateio.ws",
                                    .connect_ip = "52.198.250.74",
                                    .port = "443",
                                    .enable_tls = true,
                                    .worker_cpu_id = 6,
                                }});

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.connection_count, 1U);
  ASSERT_EQ(result.value.cycles.size(), 1U);
  EXPECT_EQ(result.value.cycles[0].cycle_index, 0U);
  ASSERT_EQ(result.value.cycles[0].connections.size(), 1U);
  EXPECT_EQ(result.value.cycles[0].connections[0].connect_ip, "52.198.250.74");
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

TEST(GateOrderSessionRttProbeTest, IocOnlySampleStartsWithIocPlace) {
  ProbeSampleFlow flow(
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      },
      ProbeOrderMode::kIoc);

  EXPECT_EQ(flow.Start(), ProbeSampleAction::kSubmitIocPlace);
  ASSERT_TRUE(flow.OnOrderSent(ProbeStage::kIocPlace,
                               gate::OrderSendResult{
                                   .status = gate::OrderSendStatus::kOk,
                                   .request_sequence = 21,
                                   .send_local_ns = 1000,
                               })
                  .ok);
  const ProbeSampleTransition transition =
      flow.OnOrderResponse(gate::OrderResponse{
          .kind = gate::OrderResponseKind::kAck,
          .local_order_id = 0x0700000000000002ULL,
          .request_sequence = 21,
          .local_receive_ns = 1700,
      });

  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kNone);
  EXPECT_EQ(flow.stats().gtc_place_status, ProbeStageStatus::kNotSubmitted);
  EXPECT_EQ(flow.stats().gtc_cancel_status, ProbeStageStatus::kNotSubmitted);
  EXPECT_EQ(flow.stats().ioc_place_ack_rtt_ns, 700);

  const ProbeSampleTransition terminal_transition =
      flow.OnOrderFeedback(OrderFeedbackEvent{
          .kind = OrderFeedbackKind::kCancelled,
          .local_order_id = 0x0700000000000002ULL,
          .cumulative_filled_quantity = 0.0,
          .left_quantity = 0.1,
          .cancelled_quantity = 0.1,
          .finish_reason = OrderFinishReason::kImmediateOrCancel,
      });

  ASSERT_TRUE(terminal_transition.ok) << terminal_transition.error;
  EXPECT_EQ(terminal_transition.action, ProbeSampleAction::kFinish);
  EXPECT_EQ(flow.stats().ioc_place_status,
            ProbeStageStatus::kTerminalConfirmed);
  EXPECT_EQ(flow.stats().ioc_close_status, ProbeStageStatus::kNotSubmitted);
}

TEST(GateOrderSessionRttProbeTest, IocOnlySampleClosesOnTerminalFillFeedback) {
  ProbeSampleFlow flow(
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      },
      ProbeOrderMode::kIoc);

  EXPECT_EQ(flow.Start(), ProbeSampleAction::kSubmitIocPlace);
  ASSERT_TRUE(flow.OnOrderSent(ProbeStage::kIocPlace,
                               gate::OrderSendResult{
                                   .status = gate::OrderSendStatus::kOk,
                                   .request_sequence = 21,
                                   .send_local_ns = 1000,
                               })
                  .ok);
  ASSERT_TRUE(flow.OnOrderResponse(gate::OrderResponse{
                                       .kind = gate::OrderResponseKind::kAck,
                                       .local_order_id = 0x0700000000000002ULL,
                                       .request_sequence = 21,
                                       .local_receive_ns = 1700,
                                   })
                  .ok);

  const ProbeSampleTransition transition =
      flow.OnOrderFeedback(OrderFeedbackEvent{
          .kind = OrderFeedbackKind::kCancelled,
          .local_order_id = 0x0700000000000002ULL,
          .cumulative_filled_quantity = 0.03,
          .left_quantity = 0.07,
          .cancelled_quantity = 0.07,
          .finish_reason = OrderFinishReason::kImmediateOrCancel,
      });

  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kSubmitIocClose);
  EXPECT_TRUE(flow.stats().unexpected_fill);
  EXPECT_TRUE(flow.stats().invalid_for_rtt_distribution);
}

TEST(GateOrderSessionRttProbeTest,
     IocOnlySampleWaitsForAckWhenNoFillFeedbackArrivesFirst) {
  ProbeSampleFlow flow(
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      },
      ProbeOrderMode::kIoc);

  EXPECT_EQ(flow.Start(), ProbeSampleAction::kSubmitIocPlace);
  ASSERT_TRUE(flow.OnOrderSent(ProbeStage::kIocPlace,
                               gate::OrderSendResult{
                                   .status = gate::OrderSendStatus::kOk,
                                   .request_sequence = 21,
                                   .send_local_ns = 1000,
                               })
                  .ok);

  ProbeSampleTransition transition = flow.OnOrderFeedback(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kCancelled,
      .local_order_id = 0x0700000000000002ULL,
      .cumulative_filled_quantity = 0.0,
      .left_quantity = 0.1,
      .cancelled_quantity = 0.1,
      .finish_reason = OrderFinishReason::kImmediateOrCancel,
  });

  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kNone);
  EXPECT_EQ(flow.stats().ioc_place_status, ProbeStageStatus::kSent);
  EXPECT_EQ(flow.stats().ioc_place_ack_rtt_ns, -1);

  transition = flow.OnOrderResponse(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kAck,
      .local_order_id = 0x0700000000000002ULL,
      .request_sequence = 21,
      .local_receive_ns = 1700,
  });

  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kFinish);
  EXPECT_EQ(flow.stats().ioc_place_status,
            ProbeStageStatus::kTerminalConfirmed);
  EXPECT_EQ(flow.stats().ioc_place_ack_rtt_ns, 700);
}

TEST(GateOrderSessionRttProbeTest, GtcOnlySampleFinishesAfterCancelAck) {
  ProbeSampleFlow flow(
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      },
      ProbeOrderMode::kGtc);

  EXPECT_EQ(flow.Start(), ProbeSampleAction::kSubmitGtcPlace);
  ASSERT_TRUE(flow.OnOrderSent(ProbeStage::kGtcPlace,
                               gate::OrderSendResult{
                                   .status = gate::OrderSendStatus::kOk,
                                   .request_sequence = 31,
                                   .send_local_ns = 1000,
                               })
                  .ok);
  ProbeSampleTransition transition = flow.OnOrderResponse(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kAck,
      .local_order_id = 0x0700000000000001ULL,
      .request_sequence = 31,
      .local_receive_ns = 1600,
  });
  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kSubmitGtcCancel);

  ASSERT_TRUE(flow.OnOrderSent(ProbeStage::kGtcCancel,
                               gate::OrderSendResult{
                                   .status = gate::OrderSendStatus::kOk,
                                   .request_sequence = 32,
                                   .send_local_ns = 2000,
                               })
                  .ok);
  transition = flow.OnOrderResponse(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kAck,
      .local_order_id = 0x0700000000000001ULL,
      .request_sequence = 32,
      .local_receive_ns = 2600,
  });

  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kFinish);
  EXPECT_EQ(flow.stats().gtc_cancel_ack_rtt_ns, 600);
  EXPECT_EQ(flow.stats().ioc_place_status, ProbeStageStatus::kNotSubmitted);
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
      .exchange_ns = 1300,
      .socket_timestamps =
          websocket::SocketTimestampingSnapshot{
              .available = true,
              .write_complete_ns = 1010,
              .tx_sched_ns = 1012,
              .tx_software_ns = 1020,
              .tx_ack_ns = 1400,
              .rx_software_ns = 1580,
              .ack_receive_local_ns = 1600,
          },
      .socket_timestamp_stages =
          websocket::SocketTimestampingStages{
              .write_complete_to_tx_software_ns = 10,
              .tx_software_to_tx_ack_ns = 380,
              .tx_ack_to_rx_software_ns = 180,
              .rx_software_to_ack_receive_ns = 20,
          },
  });
  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kSubmitGtcCancel);
  EXPECT_EQ(flow.stats().gtc_local_order_id, 0x0700000000000001ULL);
  EXPECT_EQ(flow.stats().gtc_place_request_send_local_ns, 1000);
  EXPECT_EQ(flow.stats().gtc_place_ack_receive_local_ns, 1600);
  EXPECT_EQ(flow.stats().gtc_place_ack_exchange_ns, 1300);
  EXPECT_EQ(flow.stats().gtc_place_ack_exchange_to_local_ns, 300);
  EXPECT_EQ(flow.stats().gtc_place_ack_rtt_ns, 600);
  EXPECT_EQ(flow.stats().gtc_place_status, ProbeStageStatus::kAcked);
  EXPECT_EQ(flow.stats().gtc_open_csv.ts_write_complete_ns, 1010);
  EXPECT_EQ(flow.stats().gtc_open_csv.ts_tx_sched_ns, 1012);
  EXPECT_EQ(flow.stats().gtc_open_csv.ts_tx_software_ns, 1020);
  EXPECT_EQ(flow.stats().gtc_open_csv.ts_tx_ack_ns, 1400);
  EXPECT_EQ(flow.stats().gtc_open_csv.ts_rx_software_ns, 1580);
  EXPECT_EQ(flow.stats().gtc_open_csv.ts_write_to_tx_software_ns, 10);
  EXPECT_EQ(flow.stats().gtc_open_csv.ts_tx_software_to_tx_ack_ns, 380);
  EXPECT_EQ(flow.stats().gtc_open_csv.ts_tx_ack_to_rx_software_ns, 180);
  EXPECT_EQ(flow.stats().gtc_open_csv.ts_rx_software_to_ack_receive_ns, 20);

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
      .exchange_ns = 2500,
  });
  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kSubmitIocPlace);
  EXPECT_EQ(flow.stats().gtc_cancel_request_send_local_ns, 2000);
  EXPECT_EQ(flow.stats().gtc_cancel_ack_receive_local_ns, 2900);
  EXPECT_EQ(flow.stats().gtc_cancel_ack_exchange_ns, 2500);
  EXPECT_EQ(flow.stats().gtc_cancel_ack_exchange_to_local_ns, 400);
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
      .exchange_ns = 3200,
  });
  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kSubmitIocClose);
  EXPECT_EQ(flow.stats().ioc_local_order_id, 0x0700000000000002ULL);
  EXPECT_EQ(flow.stats().ioc_place_request_send_local_ns, 3000);
  EXPECT_EQ(flow.stats().ioc_place_ack_receive_local_ns, 3700);
  EXPECT_EQ(flow.stats().ioc_place_ack_exchange_ns, 3200);
  EXPECT_EQ(flow.stats().ioc_place_ack_exchange_to_local_ns, 500);
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

TEST(GateOrderSessionRttProbeTest,
     IocOnlyExecutorDoesNotRequireValidGtcPassiveOrder) {
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
  auto result = ProbeSampleExecutor::Create(
      invalid_gtc, ioc_passive,
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      },
      ProbeOrderMode::kIoc);
  ASSERT_TRUE(result.ok) << result.error;
  ProbeSampleExecutor& executor = *result.value;
  FakeProbeOrderSession session;

  const ProbeSampleTransition transition = executor.Start(session);

  ASSERT_TRUE(transition.ok) << transition.error;
  ASSERT_EQ(session.sent_orders.size(), 1U);
  EXPECT_EQ(session.sent_orders[0].action, "place");
  EXPECT_EQ(session.sent_orders[0].order.local_order_id, 0x0700000000000002ULL);
  EXPECT_EQ(session.sent_orders[0].order.time_in_force,
            TimeInForce::kImmediateOrCancel);
}

TEST(GateOrderSessionRttProbeTest,
     GtcOnlyExecutorDoesNotRequireValidIocPassiveOrder) {
  const PassiveOrderBuildResult gtc_passive{
      .ok = true,
      .contract = "ZEC_USDT",
      .price_text = "75.0",
      .quantity_text = "0.1",
      .bbo_ticker_id = 42,
      .bbo_local_ns = 2000,
  };
  const PassiveOrderBuildResult invalid_ioc{
      .ok = false,
      .contract = "ZEC_USDT",
      .error = "missing BBO",
  };
  auto result = ProbeSampleExecutor::Create(
      gtc_passive, invalid_ioc,
      ProbeSampleLocalIds{
          .gtc_local_order_id = 0x0700000000000001ULL,
          .ioc_local_order_id = 0x0700000000000002ULL,
          .gtc_close_local_order_id = 0x0700000000000003ULL,
          .ioc_close_local_order_id = 0x0700000000000004ULL,
      },
      ProbeOrderMode::kGtc);
  ASSERT_TRUE(result.ok) << result.error;
  ProbeSampleExecutor& executor = *result.value;
  FakeProbeOrderSession session;

  const ProbeSampleTransition transition = executor.Start(session);

  ASSERT_TRUE(transition.ok) << transition.error;
  ASSERT_EQ(session.sent_orders.size(), 1U);
  EXPECT_EQ(session.sent_orders[0].action, "place");
  EXPECT_EQ(session.sent_orders[0].order.local_order_id, 0x0700000000000001ULL);
  EXPECT_EQ(session.sent_orders[0].order.time_in_force,
            TimeInForce::kGoodTillCancel);
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

TEST(GateOrderSessionRttProbeTest, DispatchesGtcSafetyCloseOnFeedbackFill) {
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

  const ProbeSampleTransition transition = executor.OnOrderFeedback(
      session, OrderFeedbackEvent{
                   .kind = OrderFeedbackKind::kPartialFilled,
                   .local_order_id = 0x0700000000000001ULL,
                   .cumulative_filled_quantity = 0.1,
                   .left_quantity = 0.0,
                   .fill_price = 75.0,
               });

  ASSERT_TRUE(transition.ok) << transition.error;
  ASSERT_EQ(session.sent_orders.size(), 3U);
  EXPECT_EQ(session.sent_orders[2].action, "place");
  EXPECT_EQ(session.sent_orders[2].order.local_order_id, 0x0700000000000003ULL);
  EXPECT_EQ(session.sent_orders[2].order.side, OrderSide::kSell);
  EXPECT_TRUE(session.sent_orders[2].order.reduce_only);
  EXPECT_TRUE(executor.stats().unexpected_fill);
  EXPECT_TRUE(executor.stats().invalid_for_rtt_distribution);
  EXPECT_NE(executor.stats().invalid_reason.find("fill"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, WaitsForSafetyCloseTerminalAfterCloseAck) {
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

  ProbeSampleTransition transition = executor.OnOrderResponse(
      session,
      gate::OrderResponse{
          .kind = gate::OrderResponseKind::kAck,
          .local_order_id = 0x0700000000000003ULL,
          .request_sequence = session.sent_orders[2].request_sequence,
          .local_receive_ns = session.sent_orders[2].send_local_ns + 800,
      });
  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kNone);
  EXPECT_EQ(executor.stats().gtc_close_status, ProbeStageStatus::kAcked);

  transition = executor.OnOrderFeedback(
      session, OrderFeedbackEvent{
                   .kind = OrderFeedbackKind::kFilled,
                   .local_order_id = 0x0700000000000003ULL,
                   .cumulative_filled_quantity = 0.1,
                   .left_quantity = 0.0,
                   .fill_price = 75.0,
               });

  ASSERT_TRUE(transition.ok) << transition.error;
  EXPECT_EQ(transition.action, ProbeSampleAction::kFinish);
  EXPECT_EQ(executor.stats().gtc_close_status,
            ProbeStageStatus::kTerminalConfirmed);
}

TEST(GateOrderSessionRttProbeTest, DispatchesSafetyCloseOnGtcCancelTimeout) {
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

  const ProbeSampleTransition transition =
      executor.OnStageTimeout(session, ProbeStage::kGtcCancel);

  ASSERT_TRUE(transition.ok) << transition.error;
  ASSERT_EQ(session.sent_orders.size(), 3U);
  EXPECT_EQ(session.sent_orders[2].action, "place");
  EXPECT_EQ(session.sent_orders[2].order.local_order_id, 0x0700000000000003ULL);
  EXPECT_TRUE(session.sent_orders[2].order.reduce_only);
  EXPECT_EQ(executor.stats().gtc_cancel_status, ProbeStageStatus::kTimeout);
  EXPECT_TRUE(executor.stats().invalid_for_rtt_distribution);
  EXPECT_NE(executor.stats().invalid_reason.find("timeout"), std::string::npos);
}

TEST(GateOrderSessionRttProbeTest, FailsSampleWhenSafetyCloseTimesOut) {
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

  const ProbeSampleTransition transition =
      executor.OnStageTimeout(session, ProbeStage::kGtcClose);

  EXPECT_FALSE(transition.ok);
  EXPECT_EQ(transition.action, ProbeSampleAction::kFail);
  EXPECT_NE(transition.error.find("safety close timeout"), std::string::npos);
  EXPECT_EQ(executor.stats().gtc_close_status, ProbeStageStatus::kTimeout);
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

TEST(GateOrderSessionRttProbeTest,
     AllocatesShardedSampleLocalOrderIdsForParallelSessions) {
  ProbeSampleIdAllocator first(/*strategy_id=*/7,
                               /*first_strategy_order_id=*/1,
                               /*strategy_order_id_stride=*/32);
  ProbeSampleIdAllocator second(/*strategy_id=*/7,
                                /*first_strategy_order_id=*/5,
                                /*strategy_order_id_stride=*/32);

  const ProbeSampleLocalIds first_ids = first.Next();
  const ProbeSampleLocalIds second_ids = second.Next();
  const ProbeSampleLocalIds first_next = first.Next();

  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(first_ids.gtc_local_order_id),
            1U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(first_ids.ioc_local_order_id),
            2U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(second_ids.gtc_local_order_id),
            5U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(second_ids.ioc_local_order_id),
            6U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(first_next.gtc_local_order_id),
            33U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(first_next.ioc_local_order_id),
            34U);
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
  websocket::SocketTimestampingConfig timestamping;
  timestamping.enabled = true;
  timestamping.tx_software = true;
  timestamping.tx_ack = true;

  const gate::OrderSessionConfig pinned = BuildPinnedOrderSessionConfig(
      base, PinnedOrderSessionOptions{
                .connect_ip = "52.198.250.74",
                .worker_cpu_id = 6,
                .enable_tcp_info_diagnostics = true,
                .timestamping = timestamping,
            });

  EXPECT_EQ(pinned.name, "base_gate_order_session");
  EXPECT_EQ(pinned.connection.host, "fx-ws.gateio.ws");
  EXPECT_EQ(pinned.connection.connect_ip, "52.198.250.74");
  EXPECT_EQ(pinned.connection.port, "443");
  EXPECT_EQ(pinned.connection.target, "/v4/ws/usdt");
  EXPECT_EQ(pinned.connection.runtime_policy.io_cpu_id, 6);
  EXPECT_TRUE(pinned.enable_tcp_info_diagnostics);
  EXPECT_TRUE(pinned.connection.socket_timestamping.enabled);
  EXPECT_TRUE(pinned.connection.socket_timestamping.tx_software);
  EXPECT_TRUE(pinned.connection.socket_timestamping.tx_ack);
}

TEST(GateOrderSessionRttProbeTest, BuildsSingleSessionLiveRunPlan) {
  ProbeConfig config;
  config.run_id = "run_1";
  config.output.root_dir = "/home/liuxiang/tmp/gate_order_session_rtt_probe";
  config.sessions.enable_tcp_info = true;
  config.sessions.timestamping.enabled = true;
  config.sessions.timestamping.tx_software = true;

  const ProbeRunPlan run_plan{
      .connection_count = 1,
      .cycles =
          {
              ProbeCycle{
                  .cycle_index = 0,
                  .group_index = 0,
                  .connections = {MakeProbeConnection("public-0",
                                                      "52.198.250.74", 6)},
              },
              ProbeCycle{
                  .cycle_index = 1,
                  .group_index = 0,
                  .connections = {MakeProbeConnection("public-0",
                                                      "52.198.250.74", 6)},
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
  EXPECT_EQ(result.value.session_name, "public-0");
  EXPECT_EQ(result.value.group, "public");
  EXPECT_EQ(result.value.connect_ip, "52.198.250.74");
  EXPECT_EQ(result.value.sample_count, 2U);
  EXPECT_EQ(result.value.order_session_config.connection.host,
            "fx-ws.gateio.ws");
  EXPECT_EQ(result.value.order_session_config.connection.connect_ip,
            "52.198.250.74");
  EXPECT_EQ(
      result.value.order_session_config.connection.runtime_policy.io_cpu_id, 6);
  EXPECT_TRUE(result.value.order_session_config.enable_tcp_info_diagnostics);
  EXPECT_TRUE(
      result.value.order_session_config.connection.socket_timestamping.enabled);
  EXPECT_TRUE(result.value.order_session_config.connection.socket_timestamping
                  .tx_software);
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

TEST(GateOrderSessionRttProbeTest, BuildsMultiSessionLiveRunPlan) {
  ProbeConfig config;
  config.run_id = "run_8";
  config.output.root_dir = "/home/liuxiang/tmp/gate_order_session_rtt_probe";
  config.sessions.enable_tcp_info = true;
  config.sessions.timestamping.enabled = true;
  config.sessions.timestamping.tx_ack = true;

  ProbeRunPlan run_plan;
  run_plan.connection_count = 8;
  for (std::uint64_t cycle_index = 0; cycle_index < 2; ++cycle_index) {
    run_plan.cycles.push_back(ProbeCycle{
        .cycle_index = cycle_index,
        .group_index = 0,
        .connections =
            {
                MakeProbeConnection("s0", "10.0.0.1", 10),
                MakeProbeConnection("s1", "10.0.0.2", 11),
                MakeProbeConnection("s2", "10.0.0.3", 12),
                MakeProbeConnection("s3", "10.0.0.4", 13),
                MakeProbeConnection("s4", "10.0.0.5", 14),
                MakeProbeConnection("s5", "10.0.0.6", 15),
                MakeProbeConnection("s6", "10.0.0.7", 16),
                MakeProbeConnection("s7", "10.0.0.8", 17),
            },
    });
  }
  gate::OrderSessionConfig base;
  base.connection.host = "fx-ws.gateio.ws";
  base.connection.port = "443";
  base.connection.target = "/v4/ws/usdt";
  base.connection.runtime_policy.io_cpu_id = 3;

  const MultiSessionLiveRunPlanResult result =
      BuildMultiSessionLiveRunPlan(config, run_plan, base);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.sessions.size(), 8U);
  EXPECT_EQ(result.value.paths.sample_csv_path,
            "/home/liuxiang/tmp/gate_order_session_rtt_probe/run_8/"
            "order_session_rtt_samples.csv");
  EXPECT_EQ(result.value.sessions[0].session_name, "s0");
  EXPECT_EQ(result.value.sessions[0].group, "public");
  EXPECT_EQ(result.value.sessions[0].connect_ip, "10.0.0.1");
  EXPECT_EQ(result.value.sessions[0].sample_count, 2U);
  EXPECT_EQ(result.value.sessions[0].order_session_id, 0U);
  EXPECT_EQ(result.value.sessions[0].local_order_id_first, 1U);
  EXPECT_EQ(result.value.sessions[0].local_order_id_stride, 32U);
  EXPECT_EQ(result.value.sessions[0].order_session_config.connection.connect_ip,
            "10.0.0.1");
  EXPECT_EQ(result.value.sessions[0]
                .order_session_config.connection.runtime_policy.io_cpu_id,
            10);
  EXPECT_TRUE(result.value.sessions[0]
                  .order_session_config.connection.socket_timestamping.enabled);
  EXPECT_TRUE(result.value.sessions[0]
                  .order_session_config.connection.socket_timestamping.tx_ack);
  EXPECT_EQ(result.value.sessions[7].connect_ip, "10.0.0.8");
  EXPECT_EQ(result.value.sessions[7].order_session_id, 7U);
  EXPECT_EQ(result.value.sessions[7].local_order_id_first, 29U);
  EXPECT_EQ(result.value.sessions[7]
                .order_session_config.connection.runtime_policy.io_cpu_id,
            17);
}

TEST(GateOrderSessionRttProbeTest,
     BuildsMultiSessionLiveRunPlanUsesConnectionEndpointFields) {
  ProbeConfig config;
  config.run_id = "run_private0";
  ProbeRunPlan run_plan;
  run_plan.connection_count = 2;
  run_plan.cycles.push_back(ProbeCycle{
      .cycle_index = 0,
      .group_index = 0,
      .connections =
          {
              ProbeConnectionConfig{
                  .name = "private-0",
                  .group = "private-10.0.1.154",
                  .host = "fxws-private.gateapi.io",
                  .connect_ip = "10.0.1.154",
                  .port = "80",
                  .enable_tls = false,
                  .worker_cpu_id = -1,
              },
              MakeProbeConnection("public-1", "13.159.186.99", -1),
          },
  });
  gate::OrderSessionConfig base;
  base.connection.host = "fx-ws.gateio.ws";
  base.connection.port = "443";
  base.connection.target = "/v4/ws/usdt";
  base.connection.enable_tls = true;

  const MultiSessionLiveRunPlanResult result =
      BuildMultiSessionLiveRunPlan(config, run_plan, base);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.sessions.size(), 2U);
  EXPECT_EQ(result.value.sessions[0].connect_ip, "10.0.1.154");
  EXPECT_EQ(result.value.sessions[0].order_session_config.connection.host,
            "fxws-private.gateapi.io");
  EXPECT_EQ(result.value.sessions[0].order_session_config.connection.port,
            "80");
  EXPECT_FALSE(
      result.value.sessions[0].order_session_config.connection.enable_tls);
  EXPECT_EQ(result.value.sessions[0].order_session_config.connection.connect_ip,
            "10.0.1.154");

  EXPECT_EQ(result.value.sessions[1].connect_ip, "13.159.186.99");
  EXPECT_EQ(result.value.sessions[1].order_session_config.connection.host,
            "fx-ws.gateio.ws");
  EXPECT_EQ(result.value.sessions[1].order_session_config.connection.port,
            "443");
  EXPECT_TRUE(
      result.value.sessions[1].order_session_config.connection.enable_tls);
  EXPECT_EQ(result.value.sessions[1].order_session_config.connection.connect_ip,
            "13.159.186.99");
}

TEST(GateOrderSessionRttProbeTest, MapsLocalOrderIdToParallelSessionIndex) {
  EXPECT_EQ(SessionIndexForLocalOrderId(
                LocalOrderIdCodec::Encode(/*strategy_id=*/7,
                                          /*strategy_order_id=*/2),
                /*session_count=*/8),
            0U);
  EXPECT_EQ(SessionIndexForLocalOrderId(
                LocalOrderIdCodec::Encode(/*strategy_id=*/7,
                                          /*strategy_order_id=*/6),
                /*session_count=*/8),
            1U);
  EXPECT_EQ(SessionIndexForLocalOrderId(
                LocalOrderIdCodec::Encode(/*strategy_id=*/7,
                                          /*strategy_order_id=*/30),
                /*session_count=*/8),
            7U);
  EXPECT_FALSE(SessionIndexForLocalOrderId(/*local_order_id=*/0,
                                           /*session_count=*/8)
                   .has_value());
  EXPECT_FALSE(SessionIndexForLocalOrderId(
                   LocalOrderIdCodec::Encode(/*strategy_id=*/7,
                                             /*strategy_order_id=*/2),
                   /*session_count=*/0)
                   .has_value());
}

TEST(GateOrderSessionRttProbeTest, LocalFeedbackQueueDrainsPushedEvents) {
  LocalFeedbackQueue queue;
  OrderFeedbackEvent first{
      .kind = OrderFeedbackKind::kCancelled,
      .local_order_id = LocalOrderIdCodec::Encode(/*strategy_id=*/7,
                                                  /*strategy_order_id=*/2),
  };
  OrderFeedbackEvent second{
      .kind = OrderFeedbackKind::kFilled,
      .local_order_id = LocalOrderIdCodec::Encode(/*strategy_id=*/7,
                                                  /*strategy_order_id=*/6),
  };
  queue.Push(first);
  queue.Push(second);

  std::vector<std::uint64_t> local_order_ids;
  EXPECT_EQ(queue.Poll(1,
                       [&](const OrderFeedbackEvent& event) {
                         local_order_ids.push_back(event.local_order_id);
                       }),
            1U);
  EXPECT_EQ(queue.Poll(8,
                       [&](const OrderFeedbackEvent& event) {
                         local_order_ids.push_back(event.local_order_id);
                       }),
            1U);
  EXPECT_EQ(queue.Poll(8,
                       [&](const OrderFeedbackEvent& event) {
                         local_order_ids.push_back(event.local_order_id);
                       }),
            0U);
  ASSERT_EQ(local_order_ids.size(), 2U);
  EXPECT_EQ(local_order_ids[0], first.local_order_id);
  EXPECT_EQ(local_order_ids[1], second.local_order_id);
}

TEST(GateOrderSessionRttProbeTest, RejectsMultiSessionLiveRunPlan) {
  ProbeConfig config;
  config.run_id = "run_1";
  const ProbeRunPlan run_plan{
      .connection_count = 2,
      .cycles =
          {
              ProbeCycle{
                  .cycle_index = 0,
                  .group_index = 0,
                  .connections =
                      {
                          MakeProbeConnection("public-0", "52.198.250.74", 6),
                          MakeProbeConnection("public-1", "52.199.212.24", 7),
                      },
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
      .session_name = "public-7",
      .group = "public",
      .connect_ip = "52.198.250.74",
      .order_session_id = 7,
      .round_index = 2,
      .sample_index = 3,
      .contract = "ZEC_USDT",
      .quantity_text = "0.1",
      .price_text = "74.9",
      .probe_order_type = "ioc",
      .order_action = "open",
      .local_order_id = 102,
      .request_sequence = 31,
      .bbo_ticker_id = 43,
      .bbo_local_ns = 1400,
      .request_send_local_ns = 1500,
      .ack_receive_local_ns = 1600,
      .ack_exchange_ns = 1550,
      .ack_exchange_to_local_ns = 50,
      .ack_rtt_ns = 100,
      .ts_write_complete_ns = 1505,
      .ts_tx_sched_ns = 1508,
      .ts_tx_software_ns = 1510,
      .ts_tx_ack_ns = 1575,
      .ts_rx_software_ns = 1585,
      .ts_write_to_tx_software_ns = 5,
      .ts_tx_software_to_tx_ack_ns = 65,
      .ts_tx_ack_to_rx_software_ns = 10,
      .ts_rx_software_to_ack_receive_ns = 15,
      .response_receive_local_ns = 1650,
      .response_exchange_ns = 1610,
      .response_exchange_to_local_ns = 40,
      .response_rtt_ns = 150,
      .status = "kTerminalConfirmed",
      .terminal_feedback_kind = "kCancelled",
      .unexpected_fill = false,
      .invalid_for_rtt_distribution = false,
      .invalid_reason = "",
  });
  writer.Close();

  EXPECT_EQ(ReadTextFileForTest(output_path),
            "run,session,group,ip,sid,round,sample,contract,qty,price,type,"
            "action,local_id,"
            "req_seq,bbo_id,bbo_ns,send_ns,ack_recv_ns,ack_ex_ns,"
            "ack_ex2local_ns,ack_rtt_ns,ts_write_complete_ns,"
            "ts_tx_sched_ns,ts_tx_software_ns,ts_tx_ack_ns,"
            "ts_rx_software_ns,ts_write_to_tx_software_ns,"
            "ts_tx_software_to_tx_ack_ns,ts_tx_ack_to_rx_software_ns,"
            "ts_rx_software_to_ack_receive_ns,resp_recv_ns,resp_ex_ns,"
            "resp_ex2local_ns,resp_rtt_ns,status,term_fb,fill,invalid,"
            "inv_reason\n"
            "run_1,public-7,public,52.198.250.74,7,2,3,ZEC_USDT,0.1,74.9,"
            "ioc,open,102,31,43,1400,1500,1600,1550,50,100,1505,1508,1510,"
            "1575,1585,5,65,10,15,1650,1610,40,150,"
            "kTerminalConfirmed,kCancelled,false,false,\n");
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

TEST(GateOrderSessionRttProbeTest, RepeatsConnectionSetForSamplesPerSession) {
  CycleScheduler scheduler(CycleSchedulerOptions{
      .connections =
          {
              MakeProbeConnection("s0", "10.0.0.1", 6),
              MakeProbeConnection("s1", "10.0.0.2", 7),
          },
      .samples_per_session = 2,
      .cycles_per_connection_generation = 1,
  });

  ASSERT_TRUE(scheduler.HasNextCycle());
  ProbeCycle first = scheduler.NextCycle();
  ASSERT_EQ(first.connections.size(), 2U);
  EXPECT_EQ(first.connections[0].name, "s0");
  EXPECT_EQ(first.connections[1].name, "s1");
  ProbeCycle second = scheduler.NextCycle();
  ASSERT_EQ(second.connections.size(), 2U);
  EXPECT_EQ(second.connections[0].name, "s0");
  EXPECT_EQ(second.connections[1].name, "s1");
  EXPECT_FALSE(scheduler.HasNextCycle());
}

TEST(GateOrderSessionRttProbeTest,
     ZeroOrderSessionIntervalWaitsForNextMarketEvent) {
  OrderSessionDispatchPacer pacer(/*order_session_interval_ms=*/0);

  EXPECT_TRUE(
      pacer.CanDispatch(/*now_ns=*/1000, /*has_new_market_event=*/false));
  pacer.MarkDispatched(/*now_ns=*/1000);
  EXPECT_FALSE(
      pacer.CanDispatch(/*now_ns=*/2000, /*has_new_market_event=*/false));
  EXPECT_TRUE(
      pacer.CanDispatch(/*now_ns=*/2000, /*has_new_market_event=*/true));
  pacer.MarkDispatchConsumed();
  EXPECT_TRUE(
      pacer.CanDispatch(/*now_ns=*/2000, /*has_new_market_event=*/false));
}

TEST(GateOrderSessionRttProbeTest,
     PositiveOrderSessionIntervalUsesNonBlockingDeadline) {
  OrderSessionDispatchPacer pacer(/*order_session_interval_ms=*/25);

  pacer.MarkDispatched(/*now_ns=*/1'000'000);
  EXPECT_FALSE(pacer.CanDispatch(/*now_ns=*/25'999'999,
                                 /*has_new_market_event=*/true));
  EXPECT_TRUE(pacer.CanDispatch(/*now_ns=*/26'000'000,
                                /*has_new_market_event=*/false));
  pacer.MarkDispatchConsumed();
  EXPECT_TRUE(pacer.CanDispatch(/*now_ns=*/26'000'000,
                                /*has_new_market_event=*/false));
}

TEST(GateOrderSessionRttProbeTest,
     MultiSessionDispatchSchedulerPacesSessionsWithinCycle) {
  MultiSessionDispatchScheduler scheduler(MultiSessionDispatchSchedulerOptions{
      .session_count = 3,
      .sample_count_per_session = 2,
      .order_session_interval_ms = 5,
      .cycle_cooldown_ms = 10,
  });
  std::vector<std::uint64_t> samples_started = {0, 0, 0};
  std::size_t session_index = 99;

  ASSERT_TRUE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                  /*now_ns=*/1'000'000, &session_index));
  EXPECT_EQ(session_index, 0U);

  samples_started[0] = 1;
  EXPECT_FALSE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                   /*now_ns=*/1'000'000, &session_index));
  EXPECT_FALSE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                   /*now_ns=*/5'999'999, &session_index));
  ASSERT_TRUE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                  /*now_ns=*/6'000'000, &session_index));
  EXPECT_EQ(session_index, 1U);

  samples_started[1] = 1;
  EXPECT_FALSE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                   /*now_ns=*/6'000'000, &session_index));
  ASSERT_TRUE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                  /*now_ns=*/11'000'000, &session_index));
  EXPECT_EQ(session_index, 2U);

  samples_started[2] = 1;
  EXPECT_FALSE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                   /*now_ns=*/11'000'000, &session_index));
  EXPECT_FALSE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                   /*now_ns=*/20'999'999, &session_index));
  ASSERT_TRUE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                  /*now_ns=*/21'000'000, &session_index));
  EXPECT_EQ(session_index, 0U);
}

TEST(GateOrderSessionRttProbeTest,
     MultiSessionDispatchSchedulerWaitsForMarketEventAtZeroInterval) {
  MultiSessionDispatchScheduler scheduler(MultiSessionDispatchSchedulerOptions{
      .session_count = 2,
      .sample_count_per_session = 1,
      .order_session_interval_ms = 0,
      .cycle_cooldown_ms = 10,
  });
  std::vector<std::uint64_t> samples_started = {0, 0};
  std::size_t session_index = 99;

  ASSERT_TRUE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                  /*now_ns=*/1'000'000, &session_index));
  EXPECT_EQ(session_index, 0U);

  samples_started[0] = 1;
  EXPECT_FALSE(scheduler.NextGrant(/*total_market_events=*/10, samples_started,
                                   /*now_ns=*/2'000'000, &session_index));
  ASSERT_TRUE(scheduler.NextGrant(/*total_market_events=*/11, samples_started,
                                  /*now_ns=*/2'000'000, &session_index));
  EXPECT_EQ(session_index, 1U);
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

TEST(GateOrderSessionRttProbeTest,
     SessionWatchdogStopsSessionWhenRunnerRequestsStop) {
  FakeWatchdogSession session;
  std::atomic<bool> runner_stop_requested{false};
  std::thread stop_thread([&] {
    while (!session.started) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    runner_stop_requested.store(true);
  });

  const SessionWatchdogResult result = RunSessionWithWatchdog(
      session, [&] { return runner_stop_requested.load(); },
      /*duration_sec=*/30.0, /*watchdog_grace_sec=*/0.0,
      std::chrono::milliseconds(1));
  stop_thread.join();

  EXPECT_TRUE(result.start_result);
  EXPECT_TRUE(result.runner_stop_observed);
  EXPECT_FALSE(result.duration_watchdog_fired);
  EXPECT_TRUE(session.stop_requested.load());
}

}  // namespace
}  // namespace aquila::tools::gate_order_session_rtt_probe
