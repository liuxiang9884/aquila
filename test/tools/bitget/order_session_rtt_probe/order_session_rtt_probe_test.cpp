#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_id.h"
#include "exchange/bitget/trading/order_session_config.h"
#include "tools/bitget/order_session_rtt_probe/config.h"
#include "tools/bitget/order_session_rtt_probe/connection_plan.h"
#include "tools/bitget/order_session_rtt_probe/passive_order_builder.h"
#include "tools/bitget/order_session_rtt_probe/run_plan.h"
#include "tools/bitget/order_session_rtt_probe/sample_flow.h"
#include "tools/bitget/order_session_rtt_probe/sample_id_allocator.h"
#include "tools/bitget/order_session_rtt_probe/session_config_builder.h"

namespace aquila::tools::bitget_order_session_rtt_probe {
namespace {

std::filesystem::path UniqueTmpPath(std::string_view stem) {
  static std::atomic<std::uint64_t> sequence{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::path{"/home/liuxiang/tmp"} /
         fmt::format("{}_{}_{}.csv", stem, now,
                     sequence.fetch_add(1, std::memory_order_relaxed));
}

std::filesystem::path WriteConnectionsCsv(std::string_view text) {
  const std::filesystem::path path = UniqueTmpPath("bitget_rtt_connections");
  std::ofstream output(path);
  output << text;
  output.close();
  return path;
}

toml::parse_result ParseMinimalConfig(std::string_view extra) {
  return toml::parse(fmt::format(
      R"toml(
[probe]
name = "bitget_order_session_rtt_probe"
run_id = "unit"

[probe.inputs]
order_session_config = "config/order_sessions/bitget_order_session.toml"
data_reader_config = "config/data_readers/bitget_order_session_rtt_probe.toml"
connections_file = "/home/liuxiang/tmp/bitget_connections.csv"

[probe.output]
root_dir = "/home/liuxiang/tmp/bitget_order_session_rtt_probe"

{}
)toml",
      extra));
}

ProbeConnectionConfig MakeConnection(std::string name, std::string connect_ip,
                                     std::int32_t worker_cpu_id) {
  return ProbeConnectionConfig{
      .name = std::move(name),
      .group = "ha",
      .host = "vip-ws-uta.bitget.com",
      .connect_ip = std::move(connect_ip),
      .port = "443",
      .enable_tls = true,
      .worker_cpu_id = worker_cpu_id,
  };
}

BookTicker MakeBitgetTicker(double bid, double ask) {
  return BookTicker{
      .id = 7,
      .symbol_id = 0,
      .exchange = Exchange::kBitget,
      .exchange_ns = 100,
      .event_ns = 200,
      .local_ns = 300,
      .bid_price = bid,
      .bid_volume = 1.0,
      .ask_price = ask,
      .ask_volume = 1.0,
  };
}

config::InstrumentInfo MakeBitgetInstrument() {
  return config::InstrumentInfo{
      .symbol_id = 0,
      .exchange = Exchange::kBitget,
      .symbol = "BTC_USDT",
      .exchange_symbol = "BTCUSDT",
      .price_tick = 0.1,
      .price_decimal_places = 1,
      .quantity_step = 0.0001,
      .quantity_decimal_places = 4,
      .min_quantity = 0.0001,
      .price_limit_down = 0.05,
  };
}

ProbeSampleLocalIds MakeSampleIds() {
  return ProbeSampleLocalIds{
      .ioc_local_order_id = LocalOrderIdCodec::Encode(7, 1),
      .close_local_order_id = LocalOrderIdCodec::Encode(7, 2),
  };
}

bitget::OrderSendResult MakeOrderSent(std::uint64_t sequence,
                                      std::int64_t send_ns) {
  return bitget::OrderSendResult{
      .status = bitget::OrderSendStatus::kOk,
      .request_sequence = sequence,
      .encoded_request_id = sequence,
      .send_local_ns = send_ns,
  };
}

bitget::OrderResponse MakeOrderResponse(bitget::OrderResponseKind kind,
                                        std::uint64_t local_order_id,
                                        std::uint64_t sequence,
                                        std::int64_t receive_ns) {
  return bitget::OrderResponse{
      .kind = kind,
      .request_type = bitget::OrderRequestType::kPlaceOrder,
      .local_order_id = local_order_id,
      .exchange_order_id = 123,
      .request_sequence = sequence,
      .error_code = kind == bitget::OrderResponseKind::kAck ? 0U : 40000U,
      .connection_id_hash = 77,
      .request_send_local_ns = receive_ns - 100,
      .local_receive_ns = receive_ns,
      .exchange_ns = receive_ns - 50,
      .ack_rtt_ns = 100,
  };
}

OrderFeedbackEvent MakeFeedback(OrderFeedbackKind kind,
                                std::uint64_t local_order_id,
                                double cumulative_fill) {
  return OrderFeedbackEvent{
      .kind = kind,
      .local_order_id = local_order_id,
      .exchange_order_id = 123,
      .cumulative_filled_quantity = cumulative_fill,
      .left_quantity = cumulative_fill == 0.0 ? 0.0001 : 0.0,
      .cancelled_quantity = cumulative_fill == 0.0 ? 0.0001 : 0.0,
      .fill_price = cumulative_fill == 0.0 ? 0.0 : 100.0,
      .exchange_update_ns = 2000,
      .local_receive_ns = 2100,
  };
}

TEST(BitgetRttProbeConfigTest, ParsesIocProbeConfig) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.sessions]
wait_login_timeout_ms = 9000
request_timeout_ms = 4000

[probe.sampling]
samples_per_session = 3
cycle_cooldown_us = 700000
order_session_interval_us = 25000
max_events_per_drain = 64
feedback_queue_capacity = 128
coordinator_cpu = 15

[probe.order]
order_mode = "ioc"
passive_price_limit_fraction = 0.5
bbo_freshness_ns = 500000000

[probe.feedback]
shm_config = "config/order_feedback/bitget_order_feedback_session.toml"
strategy_id = 7
force_claim = true
poll_budget = 32
terminal_timeout_ms = 4500
)toml"));

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "bitget_order_session_rtt_probe");
  EXPECT_EQ(result.value.sampling.samples_per_session, 3U);
  EXPECT_EQ(result.value.sampling.feedback_queue_capacity, 128U);
  EXPECT_EQ(result.value.order.order_mode, "ioc");
  EXPECT_EQ(result.value.order.bbo_freshness_ns, 500000000U);
  EXPECT_EQ(result.value.feedback.strategy_id, 7U);
  EXPECT_TRUE(result.value.feedback.force_claim);
}

TEST(BitgetRttProbeConfigTest, RejectsZeroSamples) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.sampling]
samples_per_session = 0
)toml"));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("samples_per_session"), std::string::npos);
}

TEST(BitgetRttProbeConfigTest, RejectsNonIocMode) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.order]
order_mode = "gtc"
)toml"));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("order_mode"), std::string::npos);
}

TEST(BitgetRttProbeConfigTest, AllowsZeroPacing) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.sampling]
cycle_cooldown_us = 0
order_session_interval_us = 0
)toml"));

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.sampling.cycle_cooldown_us, 0U);
  EXPECT_EQ(result.value.sampling.order_session_interval_us, 0U);
}

TEST(BitgetRttProbeConfigTest, AllowsFeedbackLaneZero) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.feedback]
strategy_id = 0
)toml"));

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.feedback.strategy_id, 0U);
}

TEST(BitgetRttProbeConnectionsTest, AllowsDnsAndPreservesDuplicateIpRows) {
  const std::filesystem::path path = WriteConnectionsCsv(
      "name,group,host,connect_ip,port,enable_tls,worker_cpu_id\n"
      "ha-0,ha,vip-ws-uta.bitget.com,,443,true,6\n"
      "ha-1,ha,vip-ws-uta.bitget.com,10.0.0.1,443,true,7\n"
      "ha-2,ha,vip-ws-uta.bitget.com,10.0.0.1,443,true,8\n");

  const ProbeConnectionsCsvResult result = LoadProbeConnectionsCsvFile(path);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.connections.size(), 3U);
  EXPECT_TRUE(result.connections[0].connect_ip.empty());
  EXPECT_EQ(result.connections[1].connect_ip, "10.0.0.1");
  EXPECT_EQ(result.connections[2].worker_cpu_id, 8);
  std::filesystem::remove(path);
}

TEST(BitgetRttProbeConnectionsTest, RejectsDuplicateNames) {
  const std::filesystem::path path = WriteConnectionsCsv(
      "name,group,host,connect_ip,port,enable_tls,worker_cpu_id\n"
      "ha-0,ha,vip-ws-uta.bitget.com,,443,true,6\n"
      "ha-0,ha,vip-ws-uta.bitget.com,,443,true,7\n");

  const ProbeConnectionsCsvResult result = LoadProbeConnectionsCsvFile(path);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("duplicates name"), std::string::npos);
  std::filesystem::remove(path);
}

TEST(BitgetRttProbeRunPlanTest, BuildsCyclesInConnectionOrder) {
  ProbeConfig config;
  config.sampling.samples_per_session = 2;
  std::vector<ProbeConnectionConfig> connections;
  connections.push_back(MakeConnection("ha-0", "", 6));
  connections.push_back(MakeConnection("ha-1", "10.0.0.1", 7));

  const ProbeRunPlanResult result =
      BuildProbeRunPlan(config, std::move(connections));

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.cycles.size(), 2U);
  ASSERT_EQ(result.value.cycles.front().connections.size(), 2U);
  EXPECT_EQ(result.value.cycles.front().connections[0].name, "ha-0");
  EXPECT_EQ(result.value.cycles.front().connections[1].name, "ha-1");
}

TEST(BitgetRttProbeRunPlanTest, PinnedConfigOverridesConnectionAndCpu) {
  bitget::OrderSessionConfig base;
  base.connection.host = "base.example";
  base.connection.port = "80";
  base.connection.enable_tls = false;
  base.connection.runtime_policy.io_cpu_id = 1;

  const bitget::OrderSessionConfig pinned = BuildPinnedOrderSessionConfig(
      std::move(base), MakeConnection("ha-0", "10.0.0.2", 9));

  EXPECT_EQ(pinned.connection.host, "vip-ws-uta.bitget.com");
  EXPECT_EQ(pinned.connection.connect_ip, "10.0.0.2");
  EXPECT_EQ(pinned.connection.port, "443");
  EXPECT_TRUE(pinned.connection.enable_tls);
  EXPECT_EQ(pinned.connection.runtime_policy.io_cpu_id, 9);
}

TEST(BitgetRttProbeOrderBuilderTest, BuildsPassiveBitgetIoc) {
  const ProbeOrderBuildResult built = BuildPassiveBuyIoc(
      MakeBitgetTicker(100.0, 100.1), MakeBitgetInstrument(), 0.5);

  ASSERT_TRUE(built.ok) << built.error;
  EXPECT_EQ(built.order.symbol, "BTCUSDT");
  EXPECT_EQ(built.order.side, OrderSide::kBuy);
  EXPECT_EQ(built.order.time_in_force, TimeInForce::kImmediateOrCancel);
  EXPECT_EQ(built.order.price_text, "97.5");
  EXPECT_EQ(built.order.quantity_text, "0.0001");
  EXPECT_FALSE(built.order.reduce_only);
  EXPECT_EQ(built.bbo_ticker_id, 7);
}

TEST(BitgetRttProbeOrderBuilderTest, BuildsMarketableSafetyClose) {
  const ProbeOrderBuildResult built = BuildSafetyCloseSellIoc(
      MakeBitgetTicker(100.0, 100.1), MakeBitgetInstrument(), 0.00019, 0.5);

  ASSERT_TRUE(built.ok) << built.error;
  EXPECT_EQ(built.order.side, OrderSide::kSell);
  EXPECT_EQ(built.order.price_text, "97.5");
  EXPECT_EQ(built.order.quantity_text, "0.0001");
  EXPECT_TRUE(built.order.reduce_only);
}

TEST(BitgetRttProbeOrderBuilderTest, RejectsForeignBookTicker) {
  BookTicker ticker = MakeBitgetTicker(100.0, 100.1);
  ticker.exchange = Exchange::kGate;

  const ProbeOrderBuildResult built =
      BuildPassiveBuyIoc(ticker, MakeBitgetInstrument(), 0.5);

  EXPECT_FALSE(built.ok);
  EXPECT_NE(built.error.find("Bitget"), std::string::npos);
}

TEST(BitgetRttProbeIdTest, RoutesPlaceAndCloseToSameSession) {
  ProbeSampleIdAllocator allocator(/*strategy_id=*/7,
                                   /*first_strategy_order_id=*/3,
                                   /*strategy_order_id_stride=*/8);
  const ProbeSampleLocalIds ids = allocator.Next();

  ASSERT_TRUE(SessionIndexForLocalOrderId(ids.ioc_local_order_id, 4));
  ASSERT_TRUE(SessionIndexForLocalOrderId(ids.close_local_order_id, 4));
  EXPECT_EQ(*SessionIndexForLocalOrderId(ids.ioc_local_order_id, 4), 1U);
  EXPECT_EQ(*SessionIndexForLocalOrderId(ids.close_local_order_id, 4), 1U);
}

TEST(BitgetRttProbeFlowTest, CompletesAfterAckThenZeroFillCancel) {
  const ProbeSampleLocalIds ids = MakeSampleIds();
  ProbeSampleFlow flow(ids);
  EXPECT_EQ(flow.Start().action, ProbeSampleAction::kSubmitIoc);
  ASSERT_TRUE(flow.OnOrderSent(MakeOrderSent(11, 1000)).ok);

  EXPECT_EQ(
      flow.OnOrderResponse(MakeOrderResponse(bitget::OrderResponseKind::kAck,
                                             ids.ioc_local_order_id, 11, 1100))
          .action,
      ProbeSampleAction::kNone);
  EXPECT_EQ(flow.OnOrderFeedback(MakeFeedback(OrderFeedbackKind::kCancelled,
                                              ids.ioc_local_order_id, 0.0))
                .action,
            ProbeSampleAction::kComplete);
  EXPECT_TRUE(flow.stats().normal_terminal_confirmed);
  EXPECT_EQ(flow.stats().ack_rtt_ns, 100);
}

TEST(BitgetRttProbeFlowTest, CompletesWhenFeedbackArrivesBeforeAck) {
  const ProbeSampleLocalIds ids = MakeSampleIds();
  ProbeSampleFlow flow(ids);
  ASSERT_TRUE(flow.Start().ok);
  ASSERT_TRUE(flow.OnOrderSent(MakeOrderSent(11, 1000)).ok);

  EXPECT_EQ(flow.OnOrderFeedback(MakeFeedback(OrderFeedbackKind::kCancelled,
                                              ids.ioc_local_order_id, 0.0))
                .action,
            ProbeSampleAction::kNone);
  EXPECT_EQ(
      flow.OnOrderResponse(MakeOrderResponse(bitget::OrderResponseKind::kAck,
                                             ids.ioc_local_order_id, 11, 1100))
          .action,
      ProbeSampleAction::kComplete);
}

TEST(BitgetRttProbeFlowTest, UnexpectedFillRequestsOneSafetyClose) {
  const ProbeSampleLocalIds ids = MakeSampleIds();
  ProbeSampleFlow flow(ids);
  ASSERT_TRUE(flow.Start().ok);
  ASSERT_TRUE(flow.OnOrderSent(MakeOrderSent(11, 1000)).ok);

  EXPECT_EQ(flow.OnOrderFeedback(MakeFeedback(OrderFeedbackKind::kPartialFilled,
                                              ids.ioc_local_order_id, 0.0001))
                .action,
            ProbeSampleAction::kSubmitSafetyClose);
  EXPECT_EQ(flow.OnOrderFeedback(MakeFeedback(OrderFeedbackKind::kPartialFilled,
                                              ids.ioc_local_order_id, 0.0001))
                .action,
            ProbeSampleAction::kNone);
  EXPECT_TRUE(flow.stats().unexpected_fill);
  EXPECT_TRUE(flow.stats().invalid);
}

TEST(BitgetRttProbeFlowTest, SafetyCloseFillConfirmsMitigation) {
  const ProbeSampleLocalIds ids = MakeSampleIds();
  ProbeSampleFlow flow(ids);
  ASSERT_TRUE(flow.Start().ok);
  ASSERT_TRUE(flow.OnOrderSent(MakeOrderSent(11, 1000)).ok);
  ASSERT_EQ(flow.OnOrderFeedback(MakeFeedback(OrderFeedbackKind::kFilled,
                                              ids.ioc_local_order_id, 0.0001))
                .action,
            ProbeSampleAction::kSubmitSafetyClose);
  ASSERT_TRUE(flow.OnSafetyCloseSent(MakeOrderSent(12, 1200)).ok);

  const ProbeSampleTransition transition = flow.OnOrderFeedback(MakeFeedback(
      OrderFeedbackKind::kFilled, ids.close_local_order_id, 0.0001));

  EXPECT_EQ(transition.action, ProbeSampleAction::kComplete);
  EXPECT_TRUE(flow.stats().safety_close_confirmed);
  EXPECT_TRUE(flow.stats().invalid);
}

TEST(BitgetRttProbeFlowTest, ShortSafetyCloseFillFailsImmediately) {
  const ProbeSampleLocalIds ids = MakeSampleIds();
  ProbeSampleFlow flow(ids);
  ASSERT_TRUE(flow.Start().ok);
  ASSERT_TRUE(flow.OnOrderSent(MakeOrderSent(11, 1000)).ok);
  ASSERT_EQ(flow.OnOrderFeedback(MakeFeedback(OrderFeedbackKind::kFilled,
                                              ids.ioc_local_order_id, 0.0002))
                .action,
            ProbeSampleAction::kSubmitSafetyClose);
  ASSERT_TRUE(flow.OnSafetyCloseSent(MakeOrderSent(12, 1200)).ok);

  const ProbeSampleTransition transition = flow.OnOrderFeedback(MakeFeedback(
      OrderFeedbackKind::kFilled, ids.close_local_order_id, 0.0001));

  EXPECT_FALSE(transition.ok);
  EXPECT_EQ(transition.action, ProbeSampleAction::kFail);
  EXPECT_NE(transition.error.find("did not prove flat"), std::string::npos);
}

TEST(BitgetRttProbeFlowTest, UnknownResultFailsWithoutTerminalAssumption) {
  const ProbeSampleLocalIds ids = MakeSampleIds();
  ProbeSampleFlow flow(ids);
  ASSERT_TRUE(flow.Start().ok);
  ASSERT_TRUE(flow.OnOrderSent(MakeOrderSent(11, 1000)).ok);

  const ProbeSampleTransition transition = flow.OnOrderResponse(
      MakeOrderResponse(bitget::OrderResponseKind::kUnknownResult,
                        ids.ioc_local_order_id, 11, 1100));

  EXPECT_FALSE(transition.ok);
  EXPECT_EQ(transition.action, ProbeSampleAction::kFail);
  EXPECT_NE(transition.error.find("unknown"), std::string::npos);
}

TEST(BitgetRttProbeFlowTest, RejectsMismatchedRequestType) {
  const ProbeSampleLocalIds ids = MakeSampleIds();
  ProbeSampleFlow flow(ids);
  ASSERT_TRUE(flow.Start().ok);
  ASSERT_TRUE(flow.OnOrderSent(MakeOrderSent(11, 1000)).ok);
  bitget::OrderResponse response = MakeOrderResponse(
      bitget::OrderResponseKind::kAck, ids.ioc_local_order_id, 11, 1100);
  response.request_type = bitget::OrderRequestType::kCancelOrder;

  const ProbeSampleTransition transition = flow.OnOrderResponse(response);

  EXPECT_FALSE(transition.ok);
  EXPECT_EQ(transition.action, ProbeSampleAction::kFail);
  EXPECT_NE(transition.error.find("request_type"), std::string::npos);
}

TEST(BitgetRttProbeFlowTest, ContinuityLostFailsActiveSample) {
  ProbeSampleFlow flow(MakeSampleIds());
  ASSERT_TRUE(flow.Start().ok);
  ASSERT_TRUE(flow.OnOrderSent(MakeOrderSent(11, 1000)).ok);
  OrderFeedbackEvent continuity{};
  continuity.kind = OrderFeedbackKind::kContinuityLost;

  const ProbeSampleTransition transition = flow.OnOrderFeedback(continuity);

  EXPECT_FALSE(transition.ok);
  EXPECT_EQ(transition.action, ProbeSampleAction::kFail);
  EXPECT_NE(transition.error.find("continuity"), std::string::npos);
}

}  // namespace
}  // namespace aquila::tools::bitget_order_session_rtt_probe
