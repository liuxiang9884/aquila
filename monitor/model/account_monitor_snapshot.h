#ifndef AQUILA_MONITOR_MODEL_ACCOUNT_MONITOR_SNAPSHOT_H_
#define AQUILA_MONITOR_MODEL_ACCOUNT_MONITOR_SNAPSHOT_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/common/types.h"

namespace aquila::monitor {

enum class OrderSource : std::uint8_t {
  kAquila,
  kExternal,
  kManual,
  kUnknown,
};

enum class SymbolHealth : std::uint8_t {
  kOk,
  kSelected,
  kDrift,
  kStale,
};

struct SymbolSummary {
  std::string_view symbol;
  double net_position{0.0};
  int open_order_count{0};
  double total_pnl{0.0};
  SymbolHealth health{SymbolHealth::kOk};
};

struct AccountBalance {
  std::string_view currency;
  double total_equity{0.0};
  double available{0.0};
  double used_margin{0.0};
  double realized_pnl{0.0};
  double unrealized_pnl{0.0};
  double total_pnl{0.0};
};

struct ServerMetric {
  std::string_view metric;
  std::string_view value;
  std::string_view state;
  std::string_view updated_time;
};

struct ProcessHealth {
  std::string_view name;
  std::string_view role;
  int pid{0};
  std::string_view uptime;
  double cpu_percent{0.0};
  double memory_percent{0.0};
  std::string_view status;
  std::string_view heartbeat;
  std::string_view updated_time;
};

struct ConnectionHealth {
  std::string_view venue;
  std::string_view channel;
  std::string_view state;
  std::string_view latency;
  std::string_view last_message;
  int reconnects{0};
  std::string_view updated_time;
};

struct RuntimeHealth {
  std::string_view server_state;
  double cpu_percent{0.0};
  double memory_percent{0.0};
  double disk_percent{0.0};
  int market_processes_up{0};
  int market_processes_total{0};
  int trading_processes_up{0};
  int trading_processes_total{0};
  int stale_count{0};
  int restart_count{0};
  std::span<const ServerMetric> server_metrics;
  std::span<const ProcessHealth> processes;
  std::span<const ConnectionHealth> connections;
};

struct MarketDataRow {
  std::string_view exchange;
  std::string_view exchange_symbol;
  std::string_view market_data_id;
  bool has_data{true};
  double last_price{0.0};
  double bid_price{0.0};
  double bid_volume{0.0};
  double ask_price{0.0};
  double ask_volume{0.0};
  double volume{0.0};
  double turnover{0.0};
  std::string_view updated_time;
};

struct MonitorOrder {
  std::string_view exchange;
  std::string_view exchange_symbol;
  OrderSource source{OrderSource::kUnknown};
  std::string_view source_label;
  OrderSide side{OrderSide::kBuy};
  int side_value{1};
  double quantity{0.0};
  double left_quantity{0.0};
  double filled_quantity{0.0};
  double price{0.0};
  double average_fill_price{0.0};
  double fee{0.0};
  std::string_view status;
  std::uint64_t exchange_order_id{0};
  std::uint64_t local_order_id{0};
  std::string_view text;
  std::string_view updated_time;
  int age_seconds{0};
};

struct PositionPnl {
  double net_position{0.0};
  double average_entry{0.0};
  double mark_price{0.0};
  double notional{0.0};
  double realized_pnl{0.0};
  double unrealized_pnl{0.0};
  double fees{0.0};
  double total_pnl{0.0};
  std::string_view exposure;
  std::string_view ledger_state;
  std::string_view drift_state;
  int ws_update_age_seconds{0};
  int rest_snapshot_age_seconds{0};
};

struct SourceMix {
  int aquila{0};
  int manual{0};
  int external{0};
  int unknown{0};
};

struct SymbolDetail {
  std::string_view symbol;
  std::span<const MarketDataRow> market_data;
  std::span<const MonitorOrder> orders;
  PositionPnl position;
  SourceMix source_mix;
};

struct AccountMonitorSnapshot {
  std::string_view title;
  std::string_view websocket_state;
  std::string_view rest_state;
  std::string_view mode;
  std::string_view selected_symbol;
  AccountBalance balance;
  RuntimeHealth runtime_health;
  std::span<const SymbolSummary> symbols;
  const SymbolDetail* selected_detail{nullptr};
  std::span<const std::string_view> events;
};

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_MODEL_ACCOUNT_MONITOR_SNAPSHOT_H_
