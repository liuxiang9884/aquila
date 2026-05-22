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

struct MonitorOrder {
  OrderSource source{OrderSource::kUnknown};
  std::string_view source_label;
  OrderSide side{OrderSide::kBuy};
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
  std::span<const SymbolSummary> symbols;
  const SymbolDetail* selected_detail{nullptr};
  std::span<const std::string_view> events;
};

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_MODEL_ACCOUNT_MONITOR_SNAPSHOT_H_
