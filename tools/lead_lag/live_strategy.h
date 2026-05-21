#ifndef AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_
#define AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_

#include <cstdint>
#include <string>

#include "core/config/strategy_config.h"
#include "strategy/lead_lag/execution_state.h"

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

struct RecoveryDiagnostics {
  strategy::leadlag::RecoveryState recovery_state{
      strategy::leadlag::RecoveryState::kNormal};
  bool needs_reconcile{false};
  bool manual_intervention{false};
  bool new_entries_paused{false};
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

[[nodiscard]] inline const char* RecoveryStateName(
    strategy::leadlag::RecoveryState state) noexcept {
  switch (state) {
    case strategy::leadlag::RecoveryState::kNormal:
      return "normal";
    case strategy::leadlag::RecoveryState::kDegradedNeedsReconcile:
      return "degraded_needs_reconcile";
    case strategy::leadlag::RecoveryState::kReconciling:
      return "reconciling";
    case strategy::leadlag::RecoveryState::kRecovered:
      return "recovered";
    case strategy::leadlag::RecoveryState::kManualIntervention:
      return "manual_intervention";
  }
  return "unknown";
}

[[nodiscard]] inline const char* BoolName(bool value) noexcept {
  return value ? "true" : "false";
}

[[nodiscard]] inline std::string FormatRecoveryDiagnosticsFields(
    const RecoveryDiagnostics& diagnostics) {
  std::string fields;
  fields.reserve(128);
  fields.append("recovery_state=");
  fields.append(RecoveryStateName(diagnostics.recovery_state));
  fields.append(" needs_reconcile=");
  fields.append(BoolName(diagnostics.needs_reconcile));
  fields.append(" manual_intervention=");
  fields.append(BoolName(diagnostics.manual_intervention));
  fields.append(" new_entries_paused=");
  fields.append(BoolName(diagnostics.new_entries_paused));
  return fields;
}

}  // namespace aquila::tools::lead_lag

#endif  // AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_
