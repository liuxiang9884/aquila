#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "core/config/instrument_catalog.h"
#include "tools/gate/fill_probe/bbo_cache.h"
#include "tools/gate/fill_probe/config.h"
#include "tools/gate/fill_probe/csv_writer.h"
#include "tools/gate/fill_probe/node_budget.h"
#include "tools/gate/fill_probe/order_math.h"
#include "tools/gate/fill_probe/state_machine.h"
#include "tools/gate/fill_probe/trigger_quote.h"

namespace {

std::string ReadWholeFileForTest(const std::filesystem::path& path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

TEST(GateFillProbeConfigTest, LoadsMinimalConfig) {
  const std::filesystem::path path =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      "gate_fill_probe_minimal_config.toml";
  std::ofstream out(path);
  out << R"toml(
[fill_probe]
name = "gate_btc_fill_probe"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
trigger_mode = "gate_direct"
max_nodes = 1000
duration_ms = 1800000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 30000
max_entry_notional_usdt = 10.0
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000
max_binance_freshness_ns = 2000000
max_gate_freshness_ns = 50000000

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data.gate]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_ogw_btc_fill_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1

[feedback]
shm_name = "aquila_ofb_btc_fill_probe_20260703"
channel_name = "orders"
force_claim = true

[output]
run_dir = "/home/liuxiang/tmp/gate_btc_fill_probe_test"
)toml";
  out.close();

  const auto result = aquila::tools::gate::fill_probe::LoadConfig(path);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.probe.symbol, "BTC_USDT");
  EXPECT_EQ(result.value.probe.symbol_id, 93);
  EXPECT_EQ(result.value.probe.trigger_mode,
            aquila::tools::gate::fill_probe::TriggerMode::kGateDirect);
  EXPECT_EQ(result.value.market_data.gate.shm_name,
            "aquila_gfusion_20260701_102201_30s_ogw24h");
  EXPECT_EQ(result.value.order_gateway.gtc_route_id, 0);
  EXPECT_EQ(result.value.order_gateway.ioc_route_id, 1);
  EXPECT_EQ(result.value.probe.max_close_retries, 3);
}

TEST(GateFillProbeConfigTest, LoadsBinanceTriggerGateQuoteConfig) {
  const std::filesystem::path path =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      "gate_fill_probe_binance_trigger_config.toml";
  std::ofstream out(path);
  out << R"toml(
[fill_probe]
name = "gate_btc_binance_trigger_gate_quote_probe"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
trigger_mode = "binance_trigger_gate_quote"
max_nodes = 300
duration_ms = 1800000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 30000
max_entry_notional_usdt = 10.0
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000
max_binance_freshness_ns = 2000000
max_gate_freshness_ns = 50000000

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data.gate]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[market_data.binance]
shm_name = "aquila_bfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_ogw_btc_binance_trigger_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1

[feedback]
shm_name = "aquila_ofb_btc_binance_trigger_probe_20260703"
channel_name = "orders"
force_claim = true

[output]
run_dir = "/home/liuxiang/tmp/gate_btc_binance_trigger_probe_test"
)toml";
  out.close();

  const auto result = aquila::tools::gate::fill_probe::LoadConfig(path);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(
      result.value.probe.trigger_mode,
      aquila::tools::gate::fill_probe::TriggerMode::kBinanceTriggerGateQuote);
  EXPECT_EQ(result.value.probe.max_binance_freshness_ns, 2000000);
  EXPECT_EQ(result.value.probe.max_gate_freshness_ns, 50000000);
  EXPECT_EQ(result.value.market_data.gate.shm_name,
            "aquila_gfusion_20260701_102201_30s_ogw24h");
  EXPECT_EQ(result.value.market_data.binance.shm_name,
            "aquila_bfusion_20260701_102201_30s_ogw24h");
}

TEST(GateFillProbeConfigTest, RejectsBinanceTriggerWithoutBinanceMarketData) {
  const std::filesystem::path path =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      "gate_fill_probe_missing_binance_config.toml";
  std::ofstream out(path);
  out << R"toml(
[fill_probe]
name = "gate_btc_binance_trigger_gate_quote_probe"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
trigger_mode = "binance_trigger_gate_quote"
max_nodes = 300
duration_ms = 1800000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 30000
max_entry_notional_usdt = 10.0
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000
max_binance_freshness_ns = 2000000
max_gate_freshness_ns = 50000000

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data.gate]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_ogw_btc_binance_trigger_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1

[feedback]
shm_name = "aquila_ofb_btc_binance_trigger_probe_20260703"
channel_name = "orders"
force_claim = true

[output]
run_dir = "/home/liuxiang/tmp/gate_btc_binance_trigger_probe_test"
)toml";
  out.close();

  const auto result = aquila::tools::gate::fill_probe::LoadConfig(path);
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("market_data.binance.shm_name is required"),
            std::string::npos);
}

TEST(GateFillProbeConfigTest, RejectsNonStringTriggerMode) {
  const std::filesystem::path path =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      "gate_fill_probe_non_string_trigger_mode.toml";
  std::ofstream out(path);
  out << R"toml(
[fill_probe]
name = "gate_btc_fill_probe"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
trigger_mode = 1
max_nodes = 1000
duration_ms = 1800000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 30000
max_entry_notional_usdt = 10.0
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000
max_binance_freshness_ns = 2000000
max_gate_freshness_ns = 50000000

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data.gate]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_ogw_btc_fill_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1

[feedback]
shm_name = "aquila_ofb_btc_fill_probe_20260703"
channel_name = "orders"
force_claim = true

[output]
run_dir = "/home/liuxiang/tmp/gate_btc_fill_probe_test"
)toml";
  out.close();

  const auto result = aquila::tools::gate::fill_probe::LoadConfig(path);
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("fill_probe.trigger_mode must be a string"),
            std::string::npos);
}

TEST(GateFillProbeConfigTest, RejectsNonPositiveUnresolvedTimeout) {
  const std::filesystem::path path =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      "gate_fill_probe_bad_unresolved_timeout.toml";
  std::ofstream out(path);
  out << R"toml(
[fill_probe]
name = "gate_btc_fill_probe"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
trigger_mode = "gate_direct"
max_nodes = 1000
duration_ms = 1800000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 0
max_entry_notional_usdt = 10.0
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000
max_binance_freshness_ns = 2000000
max_gate_freshness_ns = 50000000

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data.gate]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_ogw_btc_fill_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1

[feedback]
shm_name = "aquila_ofb_btc_fill_probe_20260703"
channel_name = "orders"
force_claim = true

[output]
run_dir = "/home/liuxiang/tmp/gate_btc_fill_probe_test"
)toml";
  out.close();

  const auto result = aquila::tools::gate::fill_probe::LoadConfig(path);
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("fill_probe.unresolved_timeout_ms must be "
                              "positive"),
            std::string::npos);
}

TEST(GateFillProbeOrderMathTest, ComputesBtcMinimumNotional) {
  const auto catalog_result = aquila::config::LoadInstrumentCatalogFromCsv(
      "config/instruments/usdt_futures_common_gate_binance_20260701.csv");
  ASSERT_TRUE(catalog_result.ok) << catalog_result.error;
  const auto* instrument =
      catalog_result.value.Find(aquila::Exchange::kGate, "BTC_USDT");
  ASSERT_NE(instrument, nullptr);

  const auto sizing =
      aquila::tools::gate::fill_probe::BuildOrderSizing(*instrument, 61513.1);
  ASSERT_TRUE(sizing.ok) << sizing.error;
  EXPECT_DOUBLE_EQ(sizing.value.quantity, 1.0);
  EXPECT_EQ(sizing.value.quantity_text, "1");
  EXPECT_NEAR(sizing.value.notional_usdt, 6.15131, 0.00001);
}

TEST(GateFillProbeOrderMathTest, ComputesStrictEntryAndAggressiveClosePrices) {
  const aquila::tools::gate::fill_probe::BboSnapshot bbo{
      .bid_price = 61513.0,
      .ask_price = 61513.1,
      .price_tick = 0.1,
  };

  const auto buy_entry =
      aquila::tools::gate::fill_probe::EntryPrice(aquila::OrderSide::kBuy, bbo);
  const auto sell_entry = aquila::tools::gate::fill_probe::EntryPrice(
      aquila::OrderSide::kSell, bbo);
  EXPECT_EQ(buy_entry.price_text, "61513.1");
  EXPECT_EQ(sell_entry.price_text, "61513");

  const auto close_sell = aquila::tools::gate::fill_probe::ClosePrice(
      aquila::OrderSide::kSell, bbo, 100);
  const auto close_buy = aquila::tools::gate::fill_probe::ClosePrice(
      aquila::OrderSide::kBuy, bbo, 100);
  EXPECT_EQ(close_sell.price_text, "60897.9");
  EXPECT_EQ(close_buy.price_text, "62128.3");
}

TEST(GateFillProbeBboCacheTest, KeepsLatestSaneTargetSymbolOnly) {
  using aquila::tools::gate::fill_probe::BboCache;
  BboCache cache(/*symbol_id=*/93, /*price_tick=*/0.1);

  aquila::BookTicker other{};
  other.id = 1;
  other.symbol_id = 384;
  other.exchange_ns = 1000;
  other.local_ns = 1100;
  other.bid_price = 10.0;
  other.ask_price = 10.1;
  cache.OnBookTicker(other);
  EXPECT_FALSE(cache.latest().has_value());

  aquila::BookTicker bad{};
  bad.id = 2;
  bad.symbol_id = 93;
  bad.exchange_ns = 1200;
  bad.local_ns = 1300;
  bad.bid_price = 10.2;
  bad.ask_price = 10.1;
  cache.OnBookTicker(bad);
  EXPECT_FALSE(cache.latest().has_value());

  aquila::BookTicker btc{};
  btc.id = 3;
  btc.symbol_id = 93;
  btc.exchange_ns = 1400;
  btc.local_ns = 1500;
  btc.bid_price = 61513.0;
  btc.bid_volume = 4.0;
  btc.ask_price = 61513.1;
  btc.ask_volume = 5.0;
  cache.OnBookTicker(btc);

  ASSERT_TRUE(cache.latest().has_value());
  EXPECT_EQ(cache.latest()->id, 3);
  EXPECT_EQ(cache.latest()->symbol_id, 93);
  EXPECT_DOUBLE_EQ(cache.latest()->price_tick, 0.1);
}

TEST(GateFillProbeTriggerQuoteTest, AcceptsFreshBinanceAndGateSnapshots) {
  using namespace aquila::tools::gate::fill_probe;
  const BboSnapshot binance{
      .id = 10,
      .symbol_id = 93,
      .exchange_ns = 1'000'000,
      .local_ns = 2'000'000,
      .bid_price = 61513.0,
      .ask_price = 61513.1,
      .price_tick = 0.1,
  };
  const BboSnapshot gate{
      .id = 20,
      .symbol_id = 93,
      .exchange_ns = 900'000,
      .local_ns = 1'900'000,
      .bid_price = 61512.9,
      .ask_price = 61513.0,
      .price_tick = 0.1,
  };
  const auto result = EvaluateTriggerQuote(
      binance, gate, /*decision_ns=*/3'000'000,
      FreshnessLimits{.max_binance_freshness_ns = 2'000'000,
                      .max_gate_freshness_ns = 50'000'000});
  ASSERT_TRUE(result.accepted);
  EXPECT_EQ(result.binance_freshness_ns, 1'000'000);
  EXPECT_EQ(result.gate_freshness_ns, 1'100'000);
  EXPECT_EQ(result.gate_exchange_delta_ns, -100'000);
  EXPECT_EQ(result.gate_local_delta_ns, -100'000);
  EXPECT_TRUE(result.skip_reason.empty());
}

TEST(GateFillProbeTriggerQuoteTest, RejectsStaleGateQuote) {
  using namespace aquila::tools::gate::fill_probe;
  const BboSnapshot binance{
      .id = 10,
      .symbol_id = 93,
      .exchange_ns = 100'000'000,
      .local_ns = 100'000'000,
      .bid_price = 61513.0,
      .ask_price = 61513.1,
      .price_tick = 0.1,
  };
  const BboSnapshot gate{
      .id = 20,
      .symbol_id = 93,
      .exchange_ns = 40'000'000,
      .local_ns = 40'000'000,
      .bid_price = 61512.9,
      .ask_price = 61513.0,
      .price_tick = 0.1,
  };
  const auto result = EvaluateTriggerQuote(
      binance, gate, /*decision_ns=*/100'000'000,
      FreshnessLimits{.max_binance_freshness_ns = 2'000'000,
                      .max_gate_freshness_ns = 50'000'000});
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.skip_reason, "stale_gate_quote");
}

TEST(GateFillProbeStateMachineTest, NodeEndsAfterIocCancelledAndGtcCancelled) {
  using namespace aquila::tools::gate::fill_probe;
  ProbeNode node = ProbeNode::Start(/*node_id=*/1, NodeSide::kBuy,
                                    /*decision_ns=*/1000);
  node.MarkEntrySubmitted(EntryKind::kGtc, /*local_order_id=*/11, 0, 1000);
  node.MarkEntrySubmitted(EntryKind::kIoc, /*local_order_id=*/12, 1, 1000);

  node.OnEntryTerminal(12, EntryResult::kCancelled, /*filled_qty=*/0.0,
                       /*fill_price=*/0.0, /*event_ns=*/1200);
  EXPECT_FALSE(node.Done());

  EXPECT_TRUE(node.GtcCancelDue(/*now_ns=*/1000 + 1000000001LL));
  node.MarkGtcCancelSubmitted(/*event_ns=*/1000 + 1000000001LL);
  node.OnEntryTerminal(11, EntryResult::kCancelled, /*filled_qty=*/0.0,
                       /*fill_price=*/0.0, /*event_ns=*/1000 + 1000100000LL);

  EXPECT_TRUE(node.Done());
  EXPECT_EQ(node.status(), NodeStatus::kCompletedNoFill);
}

TEST(GateFillProbeStateMachineTest, NodeUsesNetFlatForCompletion) {
  using namespace aquila::tools::gate::fill_probe;
  ProbeNode node = ProbeNode::Start(2, NodeSide::kSell, 2000);
  node.MarkEntrySubmitted(EntryKind::kGtc, 21, 0, 2000);
  node.MarkEntrySubmitted(EntryKind::kIoc, 22, 1, 2000);

  node.OnEntryTerminal(21, EntryResult::kFilled, 1.0, 61513.0, 2100);
  node.OnEntryTerminal(22, EntryResult::kFilled, 1.0, 61513.0, 2200);
  EXPECT_DOUBLE_EQ(node.net_position(), -2.0);
  EXPECT_FALSE(node.Done());

  node.MarkCloseSubmitted(EntryKind::kGtc, 31, 0, 2300);
  node.OnCloseFill(31, 1.0, 62128.3, 2400);
  EXPECT_FALSE(node.Done());

  node.MarkCloseSubmitted(EntryKind::kIoc, 32, 1, 2500);
  node.OnCloseFill(32, 1.0, 62128.3, 2600);
  EXPECT_TRUE(node.Done());
  EXPECT_EQ(node.status(), NodeStatus::kCompletedClosed);
}

TEST(GateFillProbeStateMachineTest, CloseRetryLimitLeavesNodeUnresolved) {
  using namespace aquila::tools::gate::fill_probe;
  ProbeNode node = ProbeNode::Start(3, NodeSide::kBuy, 3000);
  node.MarkEntrySubmitted(EntryKind::kIoc, 42, 1, 3000);
  node.OnEntryTerminal(42, EntryResult::kFilled, 1.0, 61513.1, 3100);

  for (int attempt = 0; attempt < 3; ++attempt) {
    ASSERT_TRUE(node.CloseRetryAllowed(EntryKind::kIoc, 3));
    node.MarkCloseSubmitted(EntryKind::kIoc, 50 + attempt, 1, 3200 + attempt);
    node.OnCloseTerminal(50 + attempt, CloseResult::kCancelled, 3300 + attempt);
  }

  EXPECT_FALSE(node.CloseRetryAllowed(EntryKind::kIoc, 3));
  EXPECT_FALSE(node.Done());
  EXPECT_FALSE(node.UnresolvedDue(3000 + 1000000001LL, 2'000'000'000LL));
  EXPECT_TRUE(node.UnresolvedDue(3000 + 2000000001LL, 2'000'000'000LL));
  node.MarkUnresolved(3000 + 30000000001LL);
  EXPECT_EQ(node.status(), NodeStatus::kUnresolved);
}

TEST(GateFillProbeStateMachineTest,
     LateCloseFillFromPreviousAttemptCanCompleteNode) {
  using namespace aquila::tools::gate::fill_probe;
  ProbeNode node = ProbeNode::Start(4, NodeSide::kBuy, 4000);
  node.MarkEntrySubmitted(EntryKind::kGtc, 41, 0, 4000);
  node.MarkEntrySubmitted(EntryKind::kIoc, 42, 1, 4000);
  node.OnEntryTerminal(41, EntryResult::kCancelled, 0.0, 0.0, 4050);
  node.OnEntryTerminal(42, EntryResult::kFilled, 1.0, 61513.1, 4100);

  ASSERT_TRUE(node.CloseRetryAllowed(EntryKind::kIoc, 3));
  node.MarkCloseSubmitted(EntryKind::kIoc, 50, 1, 4200);
  node.OnCloseTerminal(EntryKind::kIoc, CloseResult::kCancelled, 4300);

  ASSERT_TRUE(node.CloseRetryAllowed(EntryKind::kIoc, 3));
  node.MarkCloseSubmitted(EntryKind::kIoc, 51, 1, 4400);
  node.OnCloseFill(EntryKind::kIoc, 1.0, 61513.0, 4500);

  EXPECT_TRUE(node.Done());
  EXPECT_EQ(node.status(), NodeStatus::kCompletedClosed);
  EXPECT_DOUBLE_EQ(node.net_position(), 0.0);
}

TEST(GateFillProbeNodeBudgetTest,
     CountsOnlyNodesThatSubmitEntryOrdersAgainstMaxNodes) {
  using aquila::tools::gate::fill_probe::SubmittedNodeBudget;

  SubmittedNodeBudget budget(/*max_submitted_nodes=*/2);
  EXPECT_TRUE(budget.CanSubmitNode());
  EXPECT_EQ(budget.submitted_nodes(), 0);

  // Missing BBO and freshness-gate skips do not consume max_nodes. The caller
  // simply does not reserve a submitted node for those cases.
  EXPECT_TRUE(budget.CanSubmitNode());
  EXPECT_EQ(budget.ReserveSubmittedNode(), 1);
  EXPECT_EQ(budget.submitted_nodes(), 1);
  EXPECT_TRUE(budget.CanSubmitNode());

  EXPECT_EQ(budget.ReserveSubmittedNode(), 2);
  EXPECT_EQ(budget.submitted_nodes(), 2);
  EXPECT_FALSE(budget.CanSubmitNode());
}

TEST(GateFillProbeCsvWriterTest, WritesStableCsvFiles) {
  namespace fp = aquila::tools::gate::fill_probe;
  const std::filesystem::path dir =
      std::filesystem::path{"/home/liuxiang/tmp"} / "gate_fill_probe_csv_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  fp::CsvWriters writers(dir);
  ASSERT_TRUE(writers.Open().ok);
  writers.WriteNode(fp::NodeCsvRow{
      .run_id = "run-a",
      .node_id = 7,
      .side = "buy",
      .trigger_mode = "binance_trigger_gate_quote",
      .binance_bbo_id = 321,
      .binance_exchange_ns = 1000,
      .binance_local_ns = 1100,
      .gate_bbo_id = 123,
      .gate_exchange_ns = 900,
      .gate_local_ns = 1050,
      .bbo_id = 123,
      .decision_ns = 1200,
      .submit_ns = 1300,
      .finish_ns = 1400,
      .binance_freshness_ns = 100,
      .gate_freshness_ns = 150,
      .gate_exchange_delta_ns = -100,
      .gate_local_delta_ns = -50,
      .trigger_to_send_ns = 100,
      .bid_price = 61513.0,
      .ask_price = 61513.1,
      .status = "completed_no_fill",
  });
  writers.WriteLifecycle(fp::LifecycleCsvRow{
      .run_id = "run-a",
      .node_id = 7,
      .entry_kind = "ioc",
      .entry_local_order_id = 70,
      .entry_price = "61513.1",
      .entry_quantity = "1",
      .entry_result = "cancelled",
      .close_attribution = "none",
  });
  writers.WriteOrderEvent(fp::OrderEventCsvRow{
      .run_id = "run-a",
      .node_id = 7,
      .local_order_id = 70,
      .route_id = 1,
      .event_kind = "feedback_cancelled",
  });

  const std::string node_csv = ReadWholeFileForTest(dir / "node.csv");
  EXPECT_NE(node_csv.find("run_id,node_id,side,trigger_mode,binance_bbo_id"),
            std::string::npos);
  EXPECT_NE(node_csv.find("run-a,7,buy,binance_trigger_gate_quote,321"),
            std::string::npos);

  const std::string lifecycle_csv = ReadWholeFileForTest(dir / "lifecycle.csv");
  EXPECT_NE(lifecycle_csv.find("run-a,7,ioc,0,70,,,61513.1,1,"),
            std::string::npos);
}

}  // namespace
