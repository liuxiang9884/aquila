#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "core/trading/order_id.h"
#include "tools/bitget/gateway_smoke/config.h"
#include "tools/bitget/gateway_smoke/evidence_writer.h"
#include "tools/bitget/gateway_smoke/order_math.h"
#include "tools/bitget/gateway_smoke/state_machine.h"

namespace aquila::tools::bitget::gateway_smoke {
namespace {

std::filesystem::path WriteConfig(std::string_view overrides = {}) {
  static std::uint64_t sequence{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path path =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      fmt::format("bitget_gateway_smoke_config_{}_{}.toml", now, sequence++);
  std::ofstream output(path);
  output << R"toml(
[gateway_smoke]
name = "bitget_gateway_smoke"
run_id = "unit"
symbol = "BTC_USDT"
exchange_symbol = "BTCUSDT"
symbol_id = 0
strategy_id = 7
side = "buy"
quantity = 0.0001
passive_price_limit_fraction = 0.5
close_slippage_bps = 100
bbo_freshness_ns = 1000000000
ack_timeout_ms = 5000
terminal_timeout_ms = 10000
route_id = 0

[instrument_catalog]
file = "config/instruments/usdt_future_universe.csv"

[market_data]
shm_name = "aquila_bitget_market_data_unit"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_bitget_order_gateway_unit"
route_count = 1
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30

[feedback]
shm_name = "aquila_bitget_order_feedback_unit"
channel_name = "orders"
force_claim = false
poll_budget = 64

[output]
run_dir = "/home/liuxiang/tmp/bitget_gateway_smoke_unit"
)toml";
  output << overrides;
  output.close();
  return path;
}

BookTicker MakeTicker(double bid = 100.0, double ask = 100.1) {
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

config::InstrumentInfo MakeInstrument() {
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
      .notional_multiplier = 1.0,
      .price_limit_up = 0.05,
      .price_limit_down = 0.05,
  };
}

std::uint64_t EntryId() {
  return LocalOrderIdCodec::Encode(7, 1);
}

std::uint64_t CloseId() {
  return LocalOrderIdCodec::Encode(7, 2);
}

core::OrderResponseEvent Ack(std::uint64_t local_order_id) {
  return core::OrderResponseEvent{
      .kind = core::OrderResponseKind::kAck,
      .local_order_id = local_order_id,
      .exchange_order_id = 123,
      .route_id = 0,
      .local_receive_ns = 2'000,
  };
}

OrderFeedbackEvent Feedback(OrderFeedbackKind kind,
                            std::uint64_t local_order_id,
                            double cumulative_filled_quantity) {
  return OrderFeedbackEvent{
      .kind = kind,
      .local_order_id = local_order_id,
      .exchange_order_id = 123,
      .cumulative_filled_quantity = cumulative_filled_quantity,
      .left_quantity = cumulative_filled_quantity == 0.0 ? 0.0001 : 0.0,
      .cancelled_quantity = cumulative_filled_quantity == 0.0 ? 0.0001 : 0.0,
      .fill_price = cumulative_filled_quantity == 0.0 ? 0.0 : 100.0,
      .finish_reason = kind == OrderFeedbackKind::kCancelled
                           ? OrderFinishReason::kImmediateOrCancel
                           : OrderFinishReason::kUnknown,
      .exchange_update_ns = 2'500,
      .local_receive_ns = 3'000,
  };
}

TEST(BitgetGatewaySmokeConfigTest, LoadsOneShotContract) {
  const auto path = WriteConfig();
  const auto result = LoadConfig(path);
  std::filesystem::remove(path);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.run_id, "unit");
  EXPECT_EQ(result.value.side, OrderSide::kBuy);
  EXPECT_EQ(result.value.quantity, 0.0001);
  EXPECT_EQ(result.value.order_gateway.route_count, 1);
  EXPECT_EQ(result.value.route_id, 0);
}

TEST(BitgetGatewaySmokeConfigTest, RejectsFanoutGreaterThanOne) {
  const auto path = WriteConfig("\n[order_gateway_override]\nunused = 1\n");
  std::string text;
  {
    std::ifstream input(path);
    text.assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
  }
  const std::size_t route_count = text.find("route_count = 1");
  ASSERT_NE(route_count, std::string::npos);
  text.replace(route_count, std::string{"route_count = 1"}.size(),
               "route_count = 2");
  {
    std::ofstream output(path);
    output << text;
  }
  const auto result = LoadConfig(path);
  std::filesystem::remove(path);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("route_count must be 1"), std::string::npos);
}

TEST(BitgetGatewaySmokeConfigTest, RejectsQuantityDifferentFromMinimum) {
  GatewaySmokeConfig config;
  config.symbol = "BTC_USDT";
  config.exchange_symbol = "BTCUSDT";
  config.symbol_id = 0;
  config.quantity = 0.0002;
  const auto result = ValidateInstrumentContract(config, MakeInstrument());

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("quantity must equal instrument min_quantity"),
            std::string::npos);
}

TEST(BitgetGatewaySmokeOrderMathTest, BuildsMinimumPassiveBuyIoc) {
  const auto result = BuildEntryOrder(MakeTicker(), MakeInstrument(),
                                      OrderSide::kBuy, 0.0001, 0.5);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.price_text, "97.5");
  EXPECT_EQ(result.value.quantity_text, "0.0001");
  EXPECT_EQ(result.value.time_in_force, TimeInForce::kImmediateOrCancel);
  EXPECT_FALSE(result.value.reduce_only);
}

TEST(BitgetGatewaySmokeOrderMathTest, BuildsPassiveSellIoc) {
  const auto result = BuildEntryOrder(MakeTicker(), MakeInstrument(),
                                      OrderSide::kSell, 0.0001, 0.5);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.price_text, "102.7");
  EXPECT_EQ(result.value.side, OrderSide::kSell);
}

TEST(BitgetGatewaySmokeOrderMathTest, BuildsAggressiveReduceOnlyClose) {
  const auto result = BuildCloseOrder(MakeTicker(), MakeInstrument(),
                                      OrderSide::kSell, 0.0001, 100);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.price_text, "99.0");
  EXPECT_EQ(result.value.quantity_text, "0.0001");
  EXPECT_TRUE(result.value.reduce_only);
}

TEST(BitgetGatewaySmokeStateMachineTest, RequiresAckAndIndependentTerminal) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1'000, 0.0001);
  state.OnFeedback(Feedback(OrderFeedbackKind::kCancelled, EntryId(), 0.0));
  EXPECT_FALSE(state.done());

  state.OnGatewayResponse(Ack(EntryId()));

  EXPECT_TRUE(state.done());
  EXPECT_FALSE(state.failed());
  EXPECT_EQ(state.result(), SmokeResult::kNoFill);
}

TEST(BitgetGatewaySmokeStateMachineTest, FilledEntryRequiresFlatClose) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1'000, 0.0001);
  state.OnGatewayResponse(Ack(EntryId()));
  state.OnFeedback(Feedback(OrderFeedbackKind::kCancelled, EntryId(), 0.0001));
  ASSERT_TRUE(state.close_required());
  EXPECT_FALSE(state.done());

  state.MarkCloseSubmitted(CloseId(), 4'000, 0.0001);
  state.OnGatewayResponse(Ack(CloseId()));
  state.OnFeedback(Feedback(OrderFeedbackKind::kFilled, CloseId(), 0.0001));

  EXPECT_TRUE(state.done());
  EXPECT_FALSE(state.failed());
  EXPECT_EQ(state.result(), SmokeResult::kClosed);
}

TEST(BitgetGatewaySmokeStateMachineTest, CloseResidualFails) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1'000, 0.0002);
  state.OnGatewayResponse(Ack(EntryId()));
  state.OnFeedback(Feedback(OrderFeedbackKind::kFilled, EntryId(), 0.0002));
  state.MarkCloseSubmitted(CloseId(), 4'000, 0.0002);
  state.OnGatewayResponse(Ack(CloseId()));
  state.OnFeedback(Feedback(OrderFeedbackKind::kCancelled, CloseId(), 0.0001));

  EXPECT_TRUE(state.failed());
  EXPECT_EQ(state.failure(), SmokeFailure::kCloseResidual);
}

TEST(BitgetGatewaySmokeStateMachineTest, CloseQuantityMustMatchEntryFill) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1'000, 0.0002);
  state.OnGatewayResponse(Ack(EntryId()));
  state.OnFeedback(Feedback(OrderFeedbackKind::kFilled, EntryId(), 0.0002));

  state.MarkCloseSubmitted(CloseId(), 4'000, 0.0001);

  EXPECT_TRUE(state.failed());
  EXPECT_EQ(state.failure(), SmokeFailure::kInvalidTransition);
}

TEST(BitgetGatewaySmokeStateMachineTest, GatewayUnknownFails) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1'000, 0.0001);
  core::OrderResponseEvent unknown = Ack(EntryId());
  unknown.kind = core::OrderResponseKind::kUnknownResult;

  state.OnGatewayResponse(unknown);

  EXPECT_TRUE(state.failed());
  EXPECT_EQ(state.failure(), SmokeFailure::kGatewayUnknown);
}

TEST(BitgetGatewaySmokeStateMachineTest, ContinuityLossFailsClosed) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1'000, 0.0001);
  OrderFeedbackEvent continuity =
      Feedback(OrderFeedbackKind::kContinuityLost, 0, 0.0);

  state.OnFeedback(continuity);

  EXPECT_TRUE(state.failed());
  EXPECT_EQ(state.failure(), SmokeFailure::kFeedbackContinuityLost);
}

TEST(BitgetGatewaySmokeStateMachineTest, AckTimeoutFails) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1'000, 0.0001);

  state.CheckTimeout(6'001, 5'000, 10'000);

  EXPECT_TRUE(state.failed());
  EXPECT_EQ(state.failure(), SmokeFailure::kAckTimeout);
}

TEST(BitgetGatewaySmokeStateMachineTest, EntryOverfillFailsInvariant) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1'000, 0.0001);
  state.OnGatewayResponse(Ack(EntryId()));

  state.OnFeedback(Feedback(OrderFeedbackKind::kFilled, EntryId(), 0.0002));

  EXPECT_TRUE(state.failed());
  EXPECT_EQ(state.failure(), SmokeFailure::kQuantityInvariant);
}

TEST(BitgetGatewaySmokeEvidenceWriterTest, PersistsAckAndTerminalSeparately) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path run_dir =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      fmt::format("bitget_gateway_smoke_evidence_{}", now);
  EvidenceWriter writer(run_dir);
  ASSERT_TRUE(writer.Open().ok);
  writer.WriteEvent(EvidenceEventRow{
      .run_id = "unit",
      .event_source = "gateway",
      .event_kind = "gateway_response",
      .order_role = "entry",
      .local_order_id = EntryId(),
      .response_kind = "ack",
  });
  writer.WriteEvent(EvidenceEventRow{
      .run_id = "unit",
      .event_source = "feedback",
      .event_kind = "feedback_terminal",
      .order_role = "entry",
      .local_order_id = EntryId(),
      .feedback_kind = "cancelled",
  });
  ASSERT_TRUE(writer
                  .WriteSummary(SmokeSummary{
                      .run_id = "unit",
                      .final_result = "no_fill",
                      .entry_local_order_id = EntryId(),
                      .entry_acked = true,
                      .entry_terminal = true,
                  })
                  .ok);
  writer.Close();

  std::ifstream csv_input(run_dir / "order_event.csv");
  const std::string csv(std::istreambuf_iterator<char>(csv_input), {});
  EXPECT_NE(csv.find("gateway,gateway_response,entry"), std::string::npos);
  EXPECT_NE(csv.find(",ack,"), std::string::npos);
  EXPECT_NE(csv.find("feedback,feedback_terminal,entry"), std::string::npos);
  std::ifstream summary_input(run_dir / "summary.json");
  const std::string summary(std::istreambuf_iterator<char>(summary_input), {});
  EXPECT_NE(summary.find("\"final_result\": \"no_fill\""), std::string::npos);
  EXPECT_NE(summary.find("\"entry_acked\": true"), std::string::npos);
  std::filesystem::remove_all(run_dir);
}

}  // namespace
}  // namespace aquila::tools::bitget::gateway_smoke
