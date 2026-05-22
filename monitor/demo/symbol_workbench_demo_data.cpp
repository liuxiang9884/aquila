#include "monitor/demo/symbol_workbench_demo_data.h"

#include <array>

namespace aquila::monitor {
namespace {

constexpr std::array<SymbolSummary, 11> kSymbolSummaries{{
    {.symbol = "ROVE_USDT",
     .net_position = -8.0,
     .open_order_count = 3,
     .total_pnl = -18.4,
     .health = SymbolHealth::kDrift},
    {.symbol = "RAVE_USDT",
     .net_position = 0.0,
     .open_order_count = 2,
     .total_pnl = 3.1},
    {.symbol = "ZEC_USDT",
     .net_position = 3.0,
     .open_order_count = 4,
     .total_pnl = 38.50 + 4.29 - 0.11,
     .health = SymbolHealth::kSelected},
    {.symbol = "SIREN_USDT",
     .net_position = 0.0,
     .open_order_count = 1,
     .total_pnl = -0.6},
    {.symbol = "ETC_USDT",
     .net_position = 12.0,
     .open_order_count = 0,
     .total_pnl = 9.3},
    {.symbol = "DASH_USDT",
     .net_position = -1.0,
     .open_order_count = 1,
     .total_pnl = -2.0,
     .health = SymbolHealth::kStale},
    {.symbol = "RIVER_USDT"},
    {.symbol = "SUI_USDT",
     .net_position = 80.0,
     .open_order_count = 2,
     .total_pnl = 6.4},
    {.symbol = "INJ_USDT", .total_pnl = 1.1},
    {.symbol = "ENA_USDT",
     .net_position = -240.0,
     .open_order_count = 1,
     .total_pnl = 11.5},
    {.symbol = "BRETT_USDT"},
}};

constexpr std::array<MonitorOrder, 6> kZecOrders{{
    {.source = OrderSource::kAquila,
     .source_label = "Aquila#2",
     .side = OrderSide::kBuy,
     .quantity = 3.0,
     .filled_quantity = 3.0,
     .price = 62.10,
     .average_fill_price = 62.08,
     .fee = -0.06,
     .status = "filled",
     .exchange_order_id = 9138472801,
     .local_order_id = 144115188075855874,
     .text = "t-144115188075855874",
     .age_seconds = 188},
    {.source = OrderSource::kManual,
     .source_label = "Manual",
     .side = OrderSide::kSell,
     .quantity = 2.0,
     .left_quantity = 2.0,
     .price = 63.40,
     .status = "open",
     .exchange_order_id = 9138472901,
     .age_seconds = 134},
    {.source = OrderSource::kExternal,
     .source_label = "External",
     .side = OrderSide::kBuy,
     .quantity = 1.0,
     .left_quantity = 0.4,
     .filled_quantity = 0.6,
     .price = 61.80,
     .average_fill_price = 61.78,
     .fee = -0.01,
     .status = "partial",
     .exchange_order_id = 9138473001,
     .text = "hedger-zec-42",
     .age_seconds = 97},
    {.source = OrderSource::kAquila,
     .source_label = "Aquila#2",
     .side = OrderSide::kSell,
     .quantity = 1.0,
     .left_quantity = 1.0,
     .price = 64.00,
     .status = "open",
     .exchange_order_id = 9138473101,
     .local_order_id = 144115188075855875,
     .text = "t-144115188075855875",
     .age_seconds = 66},
    {.source = OrderSource::kManual,
     .source_label = "Manual",
     .side = OrderSide::kSell,
     .quantity = 1.0,
     .filled_quantity = 1.0,
     .price = 62.90,
     .average_fill_price = 62.91,
     .fee = -0.02,
     .status = "filled",
     .exchange_order_id = 9138473201,
     .age_seconds = 55},
    {.source = OrderSource::kExternal,
     .source_label = "External",
     .side = OrderSide::kBuy,
     .quantity = 1.0,
     .filled_quantity = 1.0,
     .price = 60.80,
     .average_fill_price = 60.82,
     .fee = -0.02,
     .status = "filled",
     .exchange_order_id = 9138473301,
     .text = "maker-bot-zec",
     .age_seconds = 44},
}};

constexpr SymbolDetail kZecDetail{
    .symbol = "ZEC_USDT",
    .orders = kZecOrders,
    .position =
        {
            .net_position = 3.0,
            .average_entry = 61.43,
            .mark_price = 62.86,
            .notional = 188.58,
            .realized_pnl = 38.50,
            .unrealized_pnl = 4.29,
            .fees = -0.11,
            .total_pnl = 38.50 + 4.29 - 0.11,
            .exposure = "long",
            .ledger_state = "estimated",
            .drift_state = "none",
            .ws_update_age_seconds = 3,
            .rest_snapshot_age_seconds = 4,
        },
    .source_mix =
        {
            .aquila = 2,
            .manual = 2,
            .external = 2,
        },
};

constexpr std::array<std::string_view, 3> kEvents{{
    "11:40:02 ZEC_USDT Aquila buy filled qty=3 avg=62.08 fee=-0.06",
    "11:40:05 ZEC_USDT Manual sell open qty=2 px=63.40",
    "11:40:09 ROVE_USDT drift: REST open order not seen in WS",
}};

}  // namespace

std::span<const SymbolSummary> DemoSymbolSummaries() noexcept {
  return kSymbolSummaries;
}

std::string_view DemoSelectedSymbol() noexcept {
  return "ZEC_USDT";
}

const SymbolDetail* DemoSelectedSymbolDetail() noexcept {
  return &kZecDetail;
}

AccountMonitorSnapshot DemoAccountMonitorSnapshot() noexcept {
  return AccountMonitorSnapshot{
      .title = "AQUILA GATE USDT ACCOUNT",
      .websocket_state = "WS connected 14ms",
      .rest_state = "REST ok 4s",
      .mode = "read-only demo",
      .selected_symbol = DemoSelectedSymbol(),
      .symbols = DemoSymbolSummaries(),
      .selected_detail = DemoSelectedSymbolDetail(),
      .events = kEvents,
  };
}

}  // namespace aquila::monitor
