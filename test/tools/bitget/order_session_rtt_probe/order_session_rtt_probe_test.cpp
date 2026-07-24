#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_feedback_shm.h"
#include "core/trading/order_id.h"
#include "exchange/bitget/trading/order_session_config.h"
#include "tools/bitget/order_session_rtt_probe/config.h"
#include "tools/bitget/order_session_rtt_probe/connection_plan.h"
#include "tools/bitget/order_session_rtt_probe/execute_support.h"
#include "tools/bitget/order_session_rtt_probe/live_runner.h"
#include "tools/bitget/order_session_rtt_probe/local_feedback_queue.h"
#include "tools/bitget/order_session_rtt_probe/passive_order_builder.h"
#include "tools/bitget/order_session_rtt_probe/run_plan.h"
#include "tools/bitget/order_session_rtt_probe/sample_csv_writer.h"
#include "tools/bitget/order_session_rtt_probe/sample_flow.h"
#include "tools/bitget/order_session_rtt_probe/sample_id_allocator.h"
#include "tools/bitget/order_session_rtt_probe/sequential_coordinator.h"
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

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
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
      .finish_reason = kind == OrderFeedbackKind::kCancelled
                           ? OrderFinishReason::kImmediateOrCancel
                           : OrderFinishReason::kUnknown,
      .exchange_update_ns = 2000,
      .local_receive_ns = 2100,
  };
}

ProbeConfig MakeRunnerConfig() {
  ProbeConfig config;
  config.run_id = "runner-unit";
  config.sessions.request_timeout_ms = 1;
  config.sampling.max_events_per_drain = 16;
  config.order.symbol = "BTC_USDT";
  config.order.bbo_freshness_ns = 1'000'000;
  config.order.passive_price_limit_fraction = 0.5;
  config.feedback.poll_budget = 16;
  config.feedback.terminal_timeout_ms = 1;
  config.feedback.strategy_id = 7;
  return config;
}

config::InstrumentCatalog MakeRunnerCatalog() {
  config::InstrumentCatalog catalog;
  catalog.Add(MakeBitgetInstrument());
  return catalog;
}

struct FakeClock {
  std::int64_t now_ns{1'000'000'000};

  static std::int64_t Now(void* context) noexcept {
    return static_cast<FakeClock*>(context)->now_ns;
  }
};

struct FakeProbeSession {
  explicit FakeProbeSession(FakeClock& clock_ref) : clock(clock_ref) {}

  bitget::OrderSendResult PlaceOrder(
      const core::OrderPlaceRequest& request) noexcept {
    orders.push_back(ProbeWireOrder{
        .local_order_id = request.local_order_id,
        .symbol = std::string(request.SymbolView()),
        .side = request.side,
        .type = request.order_type,
        .time_in_force = request.time_in_force,
        .price = request.price,
        .quantity = request.quantity,
        .quantity_text = fmt::format("{:.{}f}", request.quantity,
                                     request.quantity_decimal_places),
        .price_text =
            fmt::format("{:.{}f}", request.price, request.price_decimal_places),
        .price_decimal_places = request.price_decimal_places,
        .quantity_decimal_places = request.quantity_decimal_places,
        .reduce_only = request.reduce_only,
    });
    if (next_status != bitget::OrderSendStatus::kOk) {
      return {.status = next_status};
    }
    const std::uint64_t sequence = next_sequence++;
    return {.status = bitget::OrderSendStatus::kOk,
            .request_sequence = sequence,
            .encoded_request_id = sequence,
            .send_local_ns = clock.now_ns};
  }

  void Stop() noexcept {
    stopped = true;
  }

  FakeClock& clock;
  std::vector<ProbeWireOrder> orders;
  bitget::OrderSendStatus next_status{bitget::OrderSendStatus::kOk};
  std::uint64_t next_sequence{1};
  bool stopped{false};
};

struct FakeProbeDataReader {
  void Push(BookTicker ticker) {
    pending.push_back(ticker);
  }

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) noexcept {
    std::uint64_t count = 0;
    while (count < max_events && next_index < pending.size()) {
      handler.OnBookTicker(pending[next_index++]);
      ++count;
    }
    return count;
  }

  std::vector<BookTicker> pending;
  std::size_t next_index{0};
};

struct FakeSampleWriter {
  bool Write(const SampleCsvRow& row, std::string*) {
    rows.push_back(row);
    return write_ok;
  }

  std::vector<SampleCsvRow> rows;
  bool write_ok{true};
};

struct FakeFeedbackReader {
  template <typename Handler>
  std::size_t Poll(std::size_t, Handler&& handler) noexcept {
    handler(event);
    return 1;
  }

  OrderFeedbackEvent event;
};

class BitgetRttProbeLiveRunnerTest : public testing::Test {
 protected:
  using Runner = LiveRunner<FakeProbeSession, FakeProbeDataReader,
                            LocalFeedbackQueue, FakeSampleWriter>;

  BitgetRttProbeLiveRunnerTest()
      : session(clock),
        runner(config, connection, /*session_index=*/0, /*session_count=*/1,
               catalog, reader, feedback, writer, &clock, &FakeClock::Now) {
    runner.BindSession(session);
  }

  BookTicker FreshTicker() const {
    BookTicker ticker = MakeBitgetTicker(100.0, 100.1);
    ticker.local_ns = clock.now_ns - 100;
    return ticker;
  }

  OrderFeedbackEvent RunnerFeedback(OrderFeedbackKind kind,
                                    std::uint64_t local_order_id,
                                    double cumulative_fill) const {
    OrderFeedbackEvent event =
        MakeFeedback(kind, local_order_id, cumulative_fill);
    event.exchange_update_ns = clock.now_ns - 50;
    event.local_receive_ns = clock.now_ns;
    return event;
  }

  void StartOneSample() {
    reader.Push(FreshTicker());
    runner.OnLoginReady();
    runner.GrantDispatch();
    runner.DriveHookOnce();
    ASSERT_EQ(session.orders.size(), 1U);
  }

  ProbeConfig config{MakeRunnerConfig()};
  ProbeConnectionConfig connection{MakeConnection("ha-0", "", 6)};
  config::InstrumentCatalog catalog{MakeRunnerCatalog()};
  FakeClock clock;
  FakeProbeSession session;
  FakeProbeDataReader reader;
  LocalFeedbackQueue feedback{8};
  FakeSampleWriter writer;
  Runner runner;
};

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
symbol = "BTC_USDT"
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
  EXPECT_EQ(result.value.order.symbol, "BTC_USDT");
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

TEST(BitgetRttProbeConfigTest, RejectsEmptyOrderSymbol) {
  const ProbeConfigResult result = ParseProbeConfig(ParseMinimalConfig(R"toml(
[probe.order]
symbol = ""
)toml"));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("symbol"), std::string::npos);
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

TEST(BitgetRttProbeFlowTest, SafetyCloseCancelledDoesNotProveFlat) {
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
      OrderFeedbackKind::kCancelled, ids.close_local_order_id, 0.0));

  EXPECT_FALSE(transition.ok);
  EXPECT_EQ(transition.action, ProbeSampleAction::kFail);
  EXPECT_NE(transition.error.find("prove flat"), std::string::npos);
}

TEST(BitgetRttProbeFlowTest, SafetyCloseDirectRejectFails) {
  const ProbeSampleLocalIds ids = MakeSampleIds();
  ProbeSampleFlow flow(ids);
  ASSERT_TRUE(flow.Start().ok);
  ASSERT_TRUE(flow.OnOrderSent(MakeOrderSent(11, 1000)).ok);
  ASSERT_EQ(flow.OnOrderFeedback(MakeFeedback(OrderFeedbackKind::kFilled,
                                              ids.ioc_local_order_id, 0.0001))
                .action,
            ProbeSampleAction::kSubmitSafetyClose);
  ASSERT_TRUE(flow.OnSafetyCloseSent(MakeOrderSent(12, 1200)).ok);

  const ProbeSampleTransition transition = flow.OnOrderResponse(
      MakeOrderResponse(bitget::OrderResponseKind::kRejected,
                        ids.close_local_order_id, 12, 1300));

  EXPECT_FALSE(transition.ok);
  EXPECT_TRUE(flow.stats().close_response_observed);
  EXPECT_EQ(flow.stats().close_response_kind,
            bitget::OrderResponseKind::kRejected);
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

TEST(BitgetRttProbeFlowTest, RejectionRetainsDirectResponseEvidence) {
  const ProbeSampleLocalIds ids = MakeSampleIds();
  ProbeSampleFlow flow(ids);
  ASSERT_TRUE(flow.Start().ok);
  ASSERT_TRUE(flow.OnOrderSent(MakeOrderSent(11, 1000)).ok);

  const ProbeSampleTransition transition = flow.OnOrderResponse(
      MakeOrderResponse(bitget::OrderResponseKind::kRejected,
                        ids.ioc_local_order_id, 11, 1100));

  EXPECT_FALSE(transition.ok);
  EXPECT_TRUE(flow.stats().place_response_observed);
  EXPECT_EQ(flow.stats().response_kind, bitget::OrderResponseKind::kRejected);
  EXPECT_EQ(flow.stats().error_code, 40000U);
  EXPECT_EQ(flow.stats().connection_id_hash, 77U);
  EXPECT_EQ(flow.stats().response_receive_ns, 1100);
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

TEST(BitgetRttProbeQueueTest, RejectsPushWhenFullAndPreservesOrder) {
  LocalFeedbackQueue queue(2);
  const OrderFeedbackEvent first = MakeFeedback(
      OrderFeedbackKind::kCancelled, MakeSampleIds().ioc_local_order_id, 0.0);
  const OrderFeedbackEvent second = MakeFeedback(
      OrderFeedbackKind::kFilled, MakeSampleIds().close_local_order_id, 0.0001);
  const OrderFeedbackEvent dropped = MakeFeedback(
      OrderFeedbackKind::kRejected, LocalOrderIdCodec::Encode(7, 3), 0.0);

  EXPECT_TRUE(queue.TryPush(first));
  EXPECT_TRUE(queue.TryPush(second));
  EXPECT_FALSE(queue.TryPush(dropped));
  EXPECT_EQ(queue.dropped_count(), 1U);

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
  ASSERT_EQ(local_order_ids.size(), 2U);
  EXPECT_EQ(local_order_ids[0], first.local_order_id);
  EXPECT_EQ(local_order_ids[1], second.local_order_id);
}

TEST(BitgetRttProbeCoordinatorTest, GrantsOneSessionAtATimeInCsvOrder) {
  SequentialCoordinator coordinator(/*session_count=*/2,
                                    /*samples_per_session=*/2,
                                    /*order_session_interval_us=*/10,
                                    /*cycle_cooldown_us=*/100);

  EXPECT_EQ(coordinator.NextGrant(/*now_ns=*/0), 0U);
  EXPECT_FALSE(coordinator.NextGrant(/*now_ns=*/1).has_value());
  EXPECT_TRUE(coordinator.MarkSampleFinished(/*session_index=*/0,
                                             /*success=*/true,
                                             /*now_ns=*/100));
  EXPECT_FALSE(coordinator.NextGrant(/*now_ns=*/10'099).has_value());
  EXPECT_EQ(coordinator.NextGrant(/*now_ns=*/10'100), 1U);
  EXPECT_TRUE(coordinator.MarkSampleFinished(/*session_index=*/1,
                                             /*success=*/true,
                                             /*now_ns=*/20'000));
  EXPECT_FALSE(coordinator.NextGrant(/*now_ns=*/119'999).has_value());
  EXPECT_EQ(coordinator.NextGrant(/*now_ns=*/120'000), 0U);
  EXPECT_TRUE(coordinator.MarkSampleFinished(0, true, 120'001));
  EXPECT_EQ(coordinator.NextGrant(130'001), 1U);
  EXPECT_TRUE(coordinator.MarkSampleFinished(1, true, 130'002));
  EXPECT_TRUE(coordinator.complete());
  EXPECT_FALSE(coordinator.failed());
  EXPECT_FALSE(coordinator.NextGrant(1'000'000).has_value());
}

TEST(BitgetRttProbeCoordinatorTest, FailureStopsFutureGrants) {
  SequentialCoordinator coordinator(/*session_count=*/2,
                                    /*samples_per_session=*/1,
                                    /*order_session_interval_us=*/0,
                                    /*cycle_cooldown_us=*/0);

  ASSERT_EQ(coordinator.NextGrant(0), 0U);
  EXPECT_TRUE(coordinator.MarkSampleFinished(0, false, 1));
  EXPECT_TRUE(coordinator.failed());
  EXPECT_FALSE(coordinator.NextGrant(2).has_value());
}

TEST(BitgetRttProbeCsvTest, WritesOnlyBitgetEvidenceColumns) {
  const std::filesystem::path output_path = UniqueTmpPath("bitget_rtt_samples");
  std::filesystem::remove(output_path);

  SampleCsvWriter writer;
  std::string error;
  ASSERT_TRUE(writer.Open(output_path, &error)) << error;
  ASSERT_TRUE(writer.Write(
      SampleCsvRow{
          .run_id = "unit-run",
          .session_name = "ha-0",
          .group = "high-availability",
          .host = "vip-ws-uta.bitget.com",
          .connect_ip = "",
          .port = "443",
          .worker_cpu = 6,
          .owner_thread_cpu = 6,
          .owner_thread_tid = 123,
          .cycle_index = 2,
          .sample_index = 3,
          .local_order_id = 101,
          .close_local_order_id = 102,
          .request_sequence = 9,
          .exchange_order_id = 777,
          .symbol = "BTCUSDT",
          .side = "kBuy",
          .quantity_text = "0.0001",
          .price_text = "97.5",
          .bbo_ticker_id = 7,
          .bbo_local_ns = 900,
          .request_send_ns = 1000,
          .response_receive_ns = 1100,
          .response_exchange_ns = 1050,
          .ack_rtt_ns = 100,
          .response_kind = "kAck",
          .error_code = 0,
          .connection_id_hash = 77,
          .terminal_feedback_kind = "kCancelled",
          .terminal_feedback_local_ns = 2100,
          .terminal_feedback_exchange_ns = 2000,
          .terminal_finish_reason = "kImmediateOrCancel",
          .cumulative_fill = 0.0,
          .outcome = "normal_terminal_confirmed",
          .invalid = false,
          .unexpected_fill = false,
          .safety_close_requested = false,
          .safety_close_sent = false,
          .safety_close_confirmed = false,
          .safety_close_filled_quantity = 0.0,
          .invalid_reason = "",
      },
      &error))
      << error;
  writer.Close();

  const std::string output = ReadTextFile(output_path);
  const std::size_t header_end = output.find('\n');
  ASSERT_NE(header_end, std::string::npos);
  const std::string header = output.substr(0, header_end);
  const std::size_t row_end = output.find('\n', header_end + 1);
  ASSERT_NE(row_end, std::string::npos);
  const std::string row =
      output.substr(header_end + 1, row_end - header_end - 1);
  EXPECT_EQ(std::count(header.begin(), header.end(), ','),
            std::count(row.begin(), row.end(), ','));
  EXPECT_NE(header.find("ack_rtt_ns"), std::string::npos);
  EXPECT_NE(header.find("connection_id_hash"), std::string::npos);
  EXPECT_NE(header.find("safety_close_confirmed"), std::string::npos);
  EXPECT_EQ(header.find("x_in_time"), std::string::npos);
  EXPECT_EQ(header.find("exchange_request_ingress_ns"), std::string::npos);
  EXPECT_NE(output.find("unit-run,ha-0,high-availability"), std::string::npos);
  std::filesystem::remove(output_path);
}

TEST(BitgetRttProbeCsvTest, WritesConnectionObservationAndRunMetadata) {
  const std::filesystem::path connections_path =
      UniqueTmpPath("bitget_rtt_observed");
  const std::filesystem::path metadata_path =
      UniqueTmpPath("bitget_rtt_metadata");
  std::filesystem::remove(connections_path);
  std::filesystem::remove(metadata_path);

  ConnectionObservedCsvWriter connection_writer;
  std::string error;
  ASSERT_TRUE(connection_writer.Open(connections_path, &error)) << error;
  ASSERT_TRUE(connection_writer.Write(
      ConnectionObservedCsvRow{
          .run_id = "unit-run",
          .session_name = "ha-0",
          .group = "high-availability",
          .configured_host = "vip-ws-uta.bitget.com",
          .configured_connect_ip = "",
          .configured_port = "443",
          .worker_cpu = 6,
          .connected_at_ns = 999,
          .endpoint_available = true,
          .local_ip = "10.0.0.1",
          .local_port = 50000,
          .remote_ip = "10.0.0.2",
          .remote_port = 443,
          .owner_thread_cpu = 6,
          .owner_thread_tid = 123,
      },
      &error))
      << error;
  connection_writer.Close();

  ProbeConfig config;
  config.run_id = "unit-run";
  config.sampling.samples_per_session = 2;
  config.feedback.strategy_id = 7;
  ASSERT_TRUE(WriteRunMetadata(metadata_path, config, /*session_count=*/1,
                               "samples.csv", "connections.csv", "0123456789AB",
                               &error))
      << error;

  EXPECT_NE(ReadTextFile(connections_path)
                .find("unit-run,ha-0,high-availability,vip-ws-uta.bitget.com"),
            std::string::npos);
  const std::string metadata = ReadTextFile(metadata_path);
  EXPECT_NE(metadata.find("\"sample_csv_schema_version\": 1"),
            std::string::npos);
  EXPECT_NE(metadata.find("\"rest_guard_implemented\": false"),
            std::string::npos);
  EXPECT_NE(metadata.find("\"session_count\": 1"), std::string::npos);
  EXPECT_NE(metadata.find("\"feedback_strategy_id\": 7"), std::string::npos);
  EXPECT_NE(metadata.find("\"order_symbol\": \"BTC_USDT\""), std::string::npos);
  EXPECT_NE(metadata.find("\"client_oid_run_namespace\": \"0123456789AB\""),
            std::string::npos);
  std::filesystem::remove(connections_path);
  std::filesystem::remove(metadata_path);
}

TEST_F(BitgetRttProbeLiveRunnerTest,
       CompletesOnlyAfterAckAndZeroFillCancelledFeedback) {
  StartOneSample();
  const ProbeWireOrder& order = session.orders.front();

  clock.now_ns += 100;
  runner.OnOrderResponse(MakeOrderResponse(bitget::OrderResponseKind::kAck,
                                           order.local_order_id,
                                           /*sequence=*/1, clock.now_ns));
  EXPECT_FALSE(runner.SampleFinished());

  ASSERT_TRUE(feedback.TryPush(RunnerFeedback(OrderFeedbackKind::kCancelled,
                                              order.local_order_id, 0.0)));
  runner.DriveHookOnce();

  EXPECT_TRUE(runner.SampleFinished());
  EXPECT_FALSE(runner.failed());
  EXPECT_EQ(runner.samples_completed(), 1U);
  EXPECT_EQ(runner.orders_sent(), 1U);
  ASSERT_EQ(writer.rows.size(), 1U);
  EXPECT_EQ(writer.rows.front().outcome, "normal_terminal_confirmed");
  EXPECT_EQ(writer.rows.front().ack_rtt_ns, 100);
  EXPECT_EQ(writer.rows.front().terminal_finish_reason, "kImmediateOrCancel");
}

TEST_F(BitgetRttProbeLiveRunnerTest,
       IgnoresDuplicateResponseAndFeedbackAfterCompletion) {
  StartOneSample();
  const ProbeWireOrder order = session.orders.front();
  clock.now_ns += 100;
  const bitget::OrderResponse ack =
      MakeOrderResponse(bitget::OrderResponseKind::kAck, order.local_order_id,
                        /*sequence=*/1, clock.now_ns);
  runner.OnOrderResponse(ack);
  const OrderFeedbackEvent cancelled =
      RunnerFeedback(OrderFeedbackKind::kCancelled, order.local_order_id, 0.0);
  ASSERT_TRUE(feedback.TryPush(cancelled));
  runner.DriveHookOnce();
  ASSERT_EQ(writer.rows.size(), 1U);

  runner.OnOrderResponse(ack);
  ASSERT_TRUE(feedback.TryPush(cancelled));
  runner.DriveHookOnce();

  EXPECT_FALSE(runner.failed());
  EXPECT_EQ(writer.rows.size(), 1U);
}

TEST_F(BitgetRttProbeLiveRunnerTest, UnknownSameSessionFeedbackFailsRun) {
  StartOneSample();
  ASSERT_TRUE(feedback.TryPush(RunnerFeedback(
      OrderFeedbackKind::kAccepted, LocalOrderIdCodec::Encode(7, 99), 0.0)));

  runner.DriveHookOnce();

  EXPECT_TRUE(runner.failed());
  EXPECT_NE(runner.failure_reason().find("unmapped"), std::string::npos);
}

TEST_F(BitgetRttProbeLiveRunnerTest, WaitsForLoginAndFreshBboBeforeSending) {
  runner.GrantDispatch();
  runner.DriveHookOnce();
  EXPECT_TRUE(session.orders.empty());

  BookTicker stale = FreshTicker();
  stale.local_ns = clock.now_ns -
                   static_cast<std::int64_t>(config.order.bbo_freshness_ns) - 1;
  reader.Push(stale);
  runner.OnLoginReady();
  runner.DriveHookOnce();
  EXPECT_TRUE(session.orders.empty());

  BookTicker foreign_symbol = FreshTicker();
  foreign_symbol.symbol_id = 1;
  reader.Push(foreign_symbol);
  runner.DriveHookOnce();
  EXPECT_TRUE(session.orders.empty());
  EXPECT_FALSE(runner.failed());

  reader.Push(FreshTicker());
  runner.DriveHookOnce();
  ASSERT_EQ(session.orders.size(), 1U);
  EXPECT_EQ(session.orders.front().time_in_force,
            TimeInForce::kImmediateOrCancel);
}

TEST_F(BitgetRttProbeLiveRunnerTest, RequestTimeoutFailsAndWritesEvidence) {
  StartOneSample();
  clock.now_ns += 1'000'001;

  runner.DriveHookOnce();

  EXPECT_TRUE(runner.failed());
  EXPECT_EQ(runner.samples_failed(), 1U);
  ASSERT_EQ(writer.rows.size(), 1U);
  EXPECT_TRUE(writer.rows.front().invalid);
  EXPECT_NE(runner.failure_reason().find("timeout"), std::string::npos);
}

TEST_F(BitgetRttProbeLiveRunnerTest, MissingConfiguredInstrumentFailsPreOrder) {
  config::InstrumentCatalog empty_catalog;
  FakeSampleWriter local_writer;
  Runner local_runner(config, connection, 0, 1, empty_catalog, reader, feedback,
                      local_writer, &clock, &FakeClock::Now);
  local_runner.BindSession(session);
  local_runner.OnLoginReady();
  local_runner.GrantDispatch();
  reader.Push(FreshTicker());

  local_runner.DriveHookOnce();

  EXPECT_TRUE(local_runner.failed());
  EXPECT_TRUE(session.orders.empty());
  EXPECT_NE(local_runner.failure_reason().find("instrument"),
            std::string::npos);
}

TEST_F(BitgetRttProbeLiveRunnerTest,
       UnexpectedFillSendsExactlyOneReduceOnlyCloseAndFailsRun) {
  StartOneSample();
  const std::uint64_t place_id = session.orders.front().local_order_id;
  ASSERT_TRUE(feedback.TryPush(
      RunnerFeedback(OrderFeedbackKind::kPartialFilled, place_id, 0.0001)));
  ASSERT_TRUE(feedback.TryPush(
      RunnerFeedback(OrderFeedbackKind::kPartialFilled, place_id, 0.0001)));

  runner.DriveHookOnce();

  ASSERT_EQ(session.orders.size(), 2U);
  EXPECT_EQ(runner.orders_sent(), 2U);
  const ProbeWireOrder& close = session.orders.back();
  EXPECT_EQ(close.side, OrderSide::kSell);
  EXPECT_TRUE(close.reduce_only);
  EXPECT_EQ(close.time_in_force, TimeInForce::kImmediateOrCancel);

  clock.now_ns += 100;
  runner.OnOrderResponse(MakeOrderResponse(bitget::OrderResponseKind::kAck,
                                           close.local_order_id,
                                           /*sequence=*/2, clock.now_ns));

  ASSERT_TRUE(feedback.TryPush(RunnerFeedback(OrderFeedbackKind::kFilled,
                                              close.local_order_id, 0.0001)));
  runner.DriveHookOnce();

  EXPECT_TRUE(runner.failed());
  ASSERT_EQ(writer.rows.size(), 1U);
  EXPECT_TRUE(writer.rows.front().unexpected_fill);
  EXPECT_TRUE(writer.rows.front().safety_close_confirmed);
  EXPECT_EQ(writer.rows.front().close_request_sequence, 2U);
  EXPECT_EQ(writer.rows.front().close_response_kind, "kAck");
  EXPECT_EQ(writer.rows.front().close_connection_id_hash, 77U);
  EXPECT_EQ(writer.rows.front().outcome,
            "safety_close_confirmed_but_run_invalid");
}

TEST_F(BitgetRttProbeLiveRunnerTest,
       DelayedPlaceAckDoesNotExtendSafetyCloseDeadline) {
  StartOneSample();
  const std::uint64_t place_id = session.orders.front().local_order_id;
  const std::int64_t place_send_ns = clock.now_ns;
  ASSERT_TRUE(feedback.TryPush(
      RunnerFeedback(OrderFeedbackKind::kPartialFilled, place_id, 0.0001)));
  runner.DriveHookOnce();
  ASSERT_EQ(session.orders.size(), 2U);

  clock.now_ns += 900'000;
  bitget::OrderResponse delayed_ack = MakeOrderResponse(
      bitget::OrderResponseKind::kAck, place_id, /*sequence=*/1, clock.now_ns);
  delayed_ack.request_send_local_ns = place_send_ns;
  delayed_ack.ack_rtt_ns = clock.now_ns - place_send_ns;
  runner.OnOrderResponse(delayed_ack);
  ASSERT_FALSE(runner.failed());

  clock.now_ns += 100'001;
  runner.DriveHookOnce();

  EXPECT_TRUE(runner.failed());
  ASSERT_EQ(writer.rows.size(), 1U);
  EXPECT_TRUE(writer.rows.front().safety_close_sent);
  EXPECT_FALSE(writer.rows.front().safety_close_confirmed);
  EXPECT_NE(runner.failure_reason().find("timeout"), std::string::npos);
}

TEST_F(BitgetRttProbeLiveRunnerTest,
       LateFillFeedbackStillTriggersSafetyCloseMitigation) {
  StartOneSample();
  const std::uint64_t place_id = session.orders.front().local_order_id;
  clock.now_ns += 1'000'001;
  reader.Push(FreshTicker());
  ASSERT_TRUE(feedback.TryPush(
      RunnerFeedback(OrderFeedbackKind::kFilled, place_id, 0.0001)));

  runner.DriveHookOnce();

  ASSERT_EQ(session.orders.size(), 2U);
  EXPECT_TRUE(session.orders.back().reduce_only);
  EXPECT_FALSE(runner.failed());
  EXPECT_TRUE(runner.has_active_sample());
}

TEST_F(BitgetRttProbeLiveRunnerTest, QueueDropStopsBeforeAnyOrder) {
  LocalFeedbackQueue tiny_feedback(1);
  FakeSampleWriter local_writer;
  Runner local_runner(config, connection, 0, 1, catalog, reader, tiny_feedback,
                      local_writer, &clock, &FakeClock::Now);
  local_runner.BindSession(session);
  ASSERT_TRUE(tiny_feedback.TryPush(MakeFeedback(
      OrderFeedbackKind::kAccepted, MakeSampleIds().ioc_local_order_id, 0.0)));
  ASSERT_FALSE(tiny_feedback.TryPush(MakeFeedback(
      OrderFeedbackKind::kCancelled, MakeSampleIds().ioc_local_order_id, 0.0)));

  local_runner.OnLoginReady();
  local_runner.GrantDispatch();
  reader.Push(FreshTicker());
  local_runner.DriveHookOnce();

  EXPECT_TRUE(local_runner.failed());
  EXPECT_TRUE(session.orders.empty());
}

TEST_F(BitgetRttProbeLiveRunnerTest, LoginLossFailsActiveSample) {
  StartOneSample();

  runner.OnLoginNotReady();

  EXPECT_TRUE(runner.failed());
  EXPECT_EQ(runner.samples_failed(), 1U);
  ASSERT_EQ(writer.rows.size(), 1U);
  EXPECT_NE(writer.rows.front().invalid_reason.find("login"),
            std::string::npos);
}

TEST_F(BitgetRttProbeLiveRunnerTest,
       GracefulStopDoesNotTurnSuccessIntoFailure) {
  StartOneSample();
  const ProbeWireOrder order = session.orders.front();
  clock.now_ns += 100;
  runner.OnOrderResponse(MakeOrderResponse(bitget::OrderResponseKind::kAck,
                                           order.local_order_id,
                                           /*sequence=*/1, clock.now_ns));
  ASSERT_TRUE(feedback.TryPush(RunnerFeedback(OrderFeedbackKind::kCancelled,
                                              order.local_order_id, 0.0)));
  runner.DriveHookOnce();
  ASSERT_FALSE(runner.failed());

  runner.PrepareGracefulStop();
  runner.OnLoginNotReady();

  EXPECT_FALSE(runner.failed());
}

TEST_F(BitgetRttProbeLiveRunnerTest, ExternalAbortFailsActiveSample) {
  StartOneSample();

  runner.RequestAbort();
  runner.DriveHookOnce();

  EXPECT_TRUE(runner.failed());
  ASSERT_EQ(writer.rows.size(), 1U);
  EXPECT_NE(writer.rows.front().invalid_reason.find("external"),
            std::string::npos);
}

TEST_F(BitgetRttProbeLiveRunnerTest, ContinuityLossFailsActiveSample) {
  StartOneSample();
  OrderFeedbackEvent continuity =
      RunnerFeedback(OrderFeedbackKind::kContinuityLost, 0, 0.0);
  continuity.continuity_reason =
      OrderFeedbackContinuityReason::kSessionDisconnected;
  ASSERT_TRUE(feedback.TryPush(continuity));

  runner.DriveHookOnce();

  EXPECT_TRUE(runner.failed());
  EXPECT_EQ(session.orders.size(), 1U);
  ASSERT_EQ(writer.rows.size(), 1U);
  EXPECT_NE(writer.rows.front().invalid_reason.find("continuity"),
            std::string::npos);
}

TEST(BitgetRttProbeExecuteGuardTest,
     ExecuteRequiresDedicatedAccountConfirmation) {
  EXPECT_FALSE(ValidateExecuteGuard(
                   ExecuteGuardInput{.execute = true, .duration_sec = 30.0})
                   .ok);
  EXPECT_TRUE(
      ValidateExecuteGuard(ExecuteGuardInput{.execute = true,
                                             .confirm_dedicated_account = true,
                                             .duration_sec = 30.0})
          .ok);
}

TEST(BitgetRttProbeExecuteGuardTest, RejectsConflictingModesAndNoDeadline) {
  EXPECT_FALSE(
      ValidateExecuteGuard(ExecuteGuardInput{.execute = true,
                                             .live_preflight = true,
                                             .confirm_dedicated_account = true,
                                             .duration_sec = 30.0})
          .ok);
  EXPECT_FALSE(
      ValidateExecuteGuard(ExecuteGuardInput{.execute = true,
                                             .confirm_dedicated_account = true,
                                             .duration_sec = 0.0})
          .ok);
  EXPECT_FALSE(
      ValidateExecuteGuard(ExecuteGuardInput{.execute = true,
                                             .confirm_dedicated_account = true,
                                             .duration_sec = 86401.0})
          .ok);
  EXPECT_TRUE(ValidateExecuteGuard(ExecuteGuardInput{.live_preflight = true,
                                                     .duration_sec = 0.0})
                  .ok);
}

TEST(BitgetRttProbeShmTest, RoutesTerminalAndBroadcastsContinuity) {
  const std::string shm_name =
      fmt::format("aquila_bitget_rtt_probe_test_{}_{}", ::getpid(),
                  std::chrono::steady_clock::now().time_since_epoch().count());
  const OrderFeedbackShmConfig shm_config{
      .shm_name = shm_name,
      .channel_name = "feedback",
      .create = true,
      .remove_existing = true,
  };
  auto manager = OrderFeedbackShmManager::Create(shm_config);
  ASSERT_TRUE(manager.ok) << manager.error;
  OrderFeedbackShmPublisher publisher(manager.value.channel());
  auto reader = OrderFeedbackShmReader::Claim(
      manager.value.channel(), /*strategy_id=*/7, /*consumer_run_id=*/9001,
      /*force_claim=*/true);
  ASSERT_TRUE(reader.ok) << reader.error;

  LocalFeedbackQueue first(8);
  LocalFeedbackQueue second(8);
  std::vector<LocalFeedbackQueue*> queues{&first, &second};
  ASSERT_TRUE(publisher.Publish(MakeFeedback(
      OrderFeedbackKind::kCancelled, LocalOrderIdCodec::Encode(7, 3), 0.0)));
  RouteFeedbackResult routed =
      RouteFeedback(reader.value, queues, /*poll_budget=*/8);
  EXPECT_EQ(routed.routed, 1U);
  EXPECT_FALSE(routed.failed);
  EXPECT_EQ(first.Poll(8, [](const OrderFeedbackEvent&) noexcept {}), 0U);
  EXPECT_EQ(second.Poll(8, [](const OrderFeedbackEvent&) noexcept {}), 1U);

  ASSERT_TRUE(publisher.PublishGlobalContinuityLost(
      OrderFeedbackContinuityReason::kSessionDisconnected,
      /*local_receive_ns=*/1000));
  routed = RouteFeedback(reader.value, queues, /*poll_budget=*/8);
  EXPECT_TRUE(routed.continuity_broadcast);
  EXPECT_EQ(routed.broadcast_deliveries, 2U);
  EXPECT_FALSE(routed.failed);
  EXPECT_EQ(first.Poll(8, [](const OrderFeedbackEvent&) noexcept {}), 1U);
  EXPECT_EQ(second.Poll(8, [](const OrderFeedbackEvent&) noexcept {}), 1U);

  reader.value.Release();
  ::shm_unlink(fmt::format("/{}", shm_name).c_str());
}

TEST(BitgetRttProbeShmTest, UnmappedFeedbackFailsRouting) {
  FakeFeedbackReader reader{.event = OrderFeedbackEvent{
                                .kind = OrderFeedbackKind::kCancelled,
                                .local_order_id = 0,
                            }};
  LocalFeedbackQueue queue(8);
  std::vector<LocalFeedbackQueue*> queues{&queue};

  const RouteFeedbackResult routed =
      RouteFeedback(reader, queues, /*poll_budget=*/8);

  EXPECT_TRUE(routed.failed);
  EXPECT_EQ(routed.unrouted, 1U);
}

}  // namespace
}  // namespace aquila::tools::bitget_order_session_rtt_probe
