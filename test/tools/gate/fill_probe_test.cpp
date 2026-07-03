#include "tools/gate/fill_probe/config.h"

#include "core/config/instrument_catalog.h"
#include "tools/gate/fill_probe/order_math.h"

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

}  // namespace
