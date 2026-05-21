#ifndef AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_
#define AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_

#include <cstdint>
#include <string>

#include "core/config/strategy_config.h"

namespace aquila::tools::lead_lag {

enum class RunMode : std::uint8_t {
  kValidateOnly,
  kSignalOnly,
  kLiveOrders,
};

struct RunModeResult {
  bool ok{false};
  RunMode mode{RunMode::kValidateOnly};
  std::string error;
};

[[nodiscard]] inline RunModeResult ResolveRunMode(
    config::StrategyMode strategy_mode, bool connect_data, bool execute) {
  if (execute && strategy_mode != config::StrategyMode::kLive) {
    return {.ok = false,
            .mode = RunMode::kValidateOnly,
            .error = "strategy.mode must be live when --execute is specified"};
  }
  if (execute) {
    return {.ok = true, .mode = RunMode::kLiveOrders};
  }
  if (connect_data) {
    return {.ok = true, .mode = RunMode::kSignalOnly};
  }
  return {.ok = true, .mode = RunMode::kValidateOnly};
}

[[nodiscard]] inline const char* RunModeName(RunMode mode) noexcept {
  switch (mode) {
    case RunMode::kValidateOnly:
      return "validate_only";
    case RunMode::kSignalOnly:
      return "signal_only";
    case RunMode::kLiveOrders:
      return "live_orders";
  }
  return "unknown";
}

}  // namespace aquila::tools::lead_lag

#endif  // AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_
