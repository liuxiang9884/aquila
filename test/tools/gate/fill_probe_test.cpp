#include "tools/gate/fill_probe/config.h"

#include "core/config/instrument_catalog.h"
#include "tools/gate/fill_probe/order_math.h"
#include "tools/gate/fill_probe/state_machine.h"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace {

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

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data]
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
  EXPECT_EQ(result.value.order_gateway.gtc_route_id, 0);
  EXPECT_EQ(result.value.order_gateway.ioc_route_id, 1);
  EXPECT_EQ(result.value.probe.max_close_retries, 3);
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

TEST(GateFillProbeOrderMathTest,
     ComputesStrictEntryAndAggressiveClosePrices) {
  const aquila::tools::gate::fill_probe::BboSnapshot bbo{
      .bid_price = 61513.0,
      .ask_price = 61513.1,
      .price_tick = 0.1,
  };

  const auto buy_entry =
      aquila::tools::gate::fill_probe::EntryPrice(aquila::OrderSide::kBuy,
                                                  bbo);
  const auto sell_entry =
      aquila::tools::gate::fill_probe::EntryPrice(aquila::OrderSide::kSell,
                                                  bbo);
  EXPECT_EQ(buy_entry.price_text, "61513.1");
  EXPECT_EQ(sell_entry.price_text, "61513");

  const auto close_sell = aquila::tools::gate::fill_probe::ClosePrice(
      aquila::OrderSide::kSell, bbo, 100);
  const auto close_buy = aquila::tools::gate::fill_probe::ClosePrice(
      aquila::OrderSide::kBuy, bbo, 100);
  EXPECT_EQ(close_sell.price_text, "60897.9");
  EXPECT_EQ(close_buy.price_text, "62128.3");
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
    node.MarkCloseSubmitted(EntryKind::kIoc, 50 + attempt, 1,
                            3200 + attempt);
    node.OnCloseTerminal(50 + attempt, CloseResult::kCancelled,
                         3300 + attempt);
  }

  EXPECT_FALSE(node.CloseRetryAllowed(EntryKind::kIoc, 3));
  EXPECT_FALSE(node.Done());
  EXPECT_TRUE(node.UnresolvedDue(3000 + 30000000001LL));
  node.MarkUnresolved(3000 + 30000000001LL);
  EXPECT_EQ(node.status(), NodeStatus::kUnresolved);
}

}  // namespace
