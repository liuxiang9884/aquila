#ifndef AQUILA_MONITOR_DEMO_SYMBOL_WORKBENCH_DEMO_DATA_H_
#define AQUILA_MONITOR_DEMO_SYMBOL_WORKBENCH_DEMO_DATA_H_

#include <span>
#include <string_view>

#include "monitor/model/account_monitor_snapshot.h"

namespace aquila::monitor {

[[nodiscard]] std::span<const SymbolSummary> DemoSymbolSummaries() noexcept;
[[nodiscard]] std::string_view DemoSelectedSymbol() noexcept;
[[nodiscard]] const SymbolDetail* DemoSelectedSymbolDetail() noexcept;
[[nodiscard]] AccountMonitorSnapshot DemoAccountMonitorSnapshot() noexcept;

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_DEMO_SYMBOL_WORKBENCH_DEMO_DATA_H_
