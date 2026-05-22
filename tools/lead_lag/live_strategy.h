#ifndef AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_
#define AQUILA_TOOLS_LEAD_LAG_LIVE_STRATEGY_H_

#include <cstdint>
#include <string>
#include <utility>

#include "core/config/strategy_config.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/execution_state.h"
#include "strategy/lead_lag/strategy.h"

namespace aquila::tools::lead_lag {

inline constexpr int kContinuityLostEmergencyHandoffExitCode = 10;

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

struct LiveOrdersStrategyStats {
  std::uint64_t book_tickers{0};
  std::uint64_t order_responses{0};
  std::uint64_t order_feedbacks{0};
  bool continuity_lost_stop_requested{false};
  RecoveryDiagnostics recovery;
};

class LiveOrdersStrategy {
 public:
  explicit LiveOrdersStrategy(strategy::leadlag::Config config)
      : inner_(std::move(config)) {
    RefreshRecoveryDiagnostics();
  }

  LiveOrdersStrategy(strategy::leadlag::Config config,
                     LiveOrdersStrategyStats* stats)
      : inner_(std::move(config)),
        stats_(stats == nullptr ? &owned_stats_ : stats) {
    RefreshRecoveryDiagnostics();
  }

  template <typename ContextT>
  void OnBookTicker(const BookTicker& ticker, ContextT& context) noexcept {
    ++stats_->book_tickers;
    inner_.OnBookTicker(ticker, context);
  }

  template <typename ContextT>
  void OnOrderResponse(const core::OrderResponseEvent& event,
                       ContextT& context) noexcept {
    ++stats_->order_responses;
    inner_.OnOrderResponse(event, context);
    RefreshRecoveryDiagnostics();
  }

  template <typename ContextT>
  void OnOrderFeedback(const OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    ++stats_->order_feedbacks;
    inner_.OnOrderFeedback(event, context);
    if (event.kind == OrderFeedbackKind::kContinuityLost) {
      stats_->continuity_lost_stop_requested = true;
      inner_.RequestStop();
    }
    RefreshRecoveryDiagnostics();
  }

  [[nodiscard]] bool ShouldStop() const noexcept {
    return inner_.ShouldStop() || stats_->continuity_lost_stop_requested;
  }

  [[nodiscard]] const LiveOrdersStrategyStats& stats() const noexcept {
    return *stats_;
  }

 private:
  void RefreshRecoveryDiagnostics() noexcept {
    stats_->recovery = RecoveryDiagnostics{
        .recovery_state = inner_.recovery_state(),
        .needs_reconcile = inner_.needs_reconcile(),
        .manual_intervention = inner_.manual_intervention(),
        .new_entries_paused = inner_.new_entries_paused(),
    };
  }

  strategy::leadlag::Strategy inner_;
  LiveOrdersStrategyStats owned_stats_;
  LiveOrdersStrategyStats* stats_{&owned_stats_};
};

[[nodiscard]] inline int ResolveLiveOrdersExitCode(
    int runtime_exit_code, const LiveOrdersStrategyStats& stats) noexcept {
  if (stats.continuity_lost_stop_requested) {
    return kContinuityLostEmergencyHandoffExitCode;
  }
  return runtime_exit_code;
}

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
