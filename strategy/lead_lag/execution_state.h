#ifndef AQUILA_STRATEGY_LEAD_LAG_EXECUTION_STATE_H_
#define AQUILA_STRATEGY_LEAD_LAG_EXECUTION_STATE_H_

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "strategy/lead_lag/config.h"

namespace aquila::strategy::leadlag {

inline constexpr double kExecutionQuantityEpsilon = 1e-12;

enum class ExecutionStage : std::uint8_t {
  kIdle,
  kOpen,
  kHold,
  kClose,
};

enum class CloseOrderKind : std::uint8_t {
  kNone,
  kNormal,
  kStoploss,
};

enum class ExecutionApplyResult : std::uint8_t {
  kIgnoredUnknownOrder,
  kIgnoredNonTerminal,
  kAppliedHold,
  kAppliedDeleted,
};

enum class RecoveryState : std::uint8_t {
  kNormal,
  kDegradedNeedsReconcile,
  kReconciling,
  kRecovered,
  kManualIntervention,
};

struct RecoveryApplyResult {
  bool recovered{false};
  bool position_match{false};
  bool open_orders_resolved{false};
  bool terminal_facts_resolved{false};
  bool manual_intervention{false};
};

[[nodiscard]] constexpr bool RecoveryApplySucceeded(
    const RecoveryApplyResult& result) noexcept {
  return result.recovered && result.position_match &&
         result.open_orders_resolved && result.terminal_facts_resolved &&
         !result.manual_intervention;
}

[[nodiscard]] constexpr bool RecoveryStateNeedsReconcile(
    RecoveryState state) noexcept {
  switch (state) {
    case RecoveryState::kDegradedNeedsReconcile:
    case RecoveryState::kReconciling:
    case RecoveryState::kManualIntervention:
      return true;
    case RecoveryState::kNormal:
    case RecoveryState::kRecovered:
      return false;
  }
  return true;
}

[[nodiscard]] constexpr bool RecoveryStateManualIntervention(
    RecoveryState state) noexcept {
  return state == RecoveryState::kManualIntervention;
}

[[nodiscard]] constexpr bool RecoveryStatePausesNewEntries(
    RecoveryState state) noexcept {
  switch (state) {
    case RecoveryState::kDegradedNeedsReconcile:
    case RecoveryState::kReconciling:
    case RecoveryState::kManualIntervention:
      return true;
    case RecoveryState::kNormal:
    case RecoveryState::kRecovered:
      return false;
  }
  return true;
}

[[nodiscard]] constexpr std::uint8_t RecoveryStateSeverity(
    RecoveryState state) noexcept {
  switch (state) {
    case RecoveryState::kNormal:
      return 0;
    case RecoveryState::kRecovered:
      return 1;
    case RecoveryState::kDegradedNeedsReconcile:
      return 2;
    case RecoveryState::kReconciling:
      return 3;
    case RecoveryState::kManualIntervention:
      return 4;
  }
  return 4;
}

[[nodiscard]] constexpr RecoveryState MoreSevereRecoveryState(
    RecoveryState lhs, RecoveryState rhs) noexcept {
  return RecoveryStateSeverity(lhs) >= RecoveryStateSeverity(rhs) ? lhs : rhs;
}

class SignalEngine;

struct ExecutionGroup {
  ExecutionStage stage{ExecutionStage::kIdle};
  std::uint64_t local_order_id{0};
  std::uint64_t entry_local_order_id{0};
  std::array<std::uint64_t, kMaxOrderSessionFanout> pending_local_order_ids{};
  std::array<std::uint64_t, kMaxOrderSessionFanout>
      unknown_result_local_order_ids{};
  double signed_position_quantity{0.0};
  double absolute_entry_value{0.0};
  double trailing_price{0.0};
  std::uint64_t group_id{0};
  std::uint8_t pending_order_count{0};
  std::uint8_t unknown_result_pending_count{0};
  CloseOrderKind close_order_kind{CloseOrderKind::kNone};
  std::uint32_t normal_close_retry_count{0};

  [[nodiscard]] bool active() const noexcept {
    return stage != ExecutionStage::kIdle;
  }

  [[nodiscard]] bool hold() const noexcept {
    return stage == ExecutionStage::kHold;
  }

  [[nodiscard]] bool pending_order() const noexcept {
    return pending_order_count != 0;
  }

  [[nodiscard]] bool long_position() const noexcept {
    return signed_position_quantity > kExecutionQuantityEpsilon;
  }

  [[nodiscard]] bool short_position() const noexcept {
    return signed_position_quantity < -kExecutionQuantityEpsilon;
  }

  [[nodiscard]] bool CanSubmitNormalClose(
      std::uint32_t close_retry_times) const noexcept {
    return normal_close_retry_count <= close_retry_times;
  }
};

class ExecutionState {
 public:
  void Init(std::uint32_t parallel) {
    groups_.assign(parallel, ExecutionGroup{});
    active_group_count_ = 0;
    next_group_id_ = 1;
    recovery_state_ = RecoveryState::kNormal;
    needs_reconcile_ = false;
    unknown_result_pending_count_ = 0;
    unknown_result_reconcile_active_ = false;
    last_unknown_result_auto_recovered_ = false;
  }

  [[nodiscard]] ExecutionGroup* StartOpenOrder(
      std::uint64_t local_order_id) noexcept {
    ExecutionGroup* group = StartOpenGroup();
    if (group == nullptr) {
      return nullptr;
    }
    if (!AddOpenOrder(*group, local_order_id)) {
      ClearGroup(*group);
      return nullptr;
    }
    return group;
  }

  [[nodiscard]] ExecutionGroup* StartOpenGroup() noexcept {
    ExecutionGroup* group = FindIdleGroup();
    if (group == nullptr) {
      return nullptr;
    }
    *group = ExecutionGroup{
        .stage = ExecutionStage::kOpen,
        .group_id = next_group_id_++,
    };
    ++active_group_count_;
    return group;
  }

  [[nodiscard]] bool AddOpenOrder(ExecutionGroup& group,
                                  std::uint64_t local_order_id) noexcept {
    if (group.stage != ExecutionStage::kOpen || local_order_id == 0) {
      return false;
    }
    if (!AddPendingOrder(group, local_order_id)) {
      return false;
    }
    if (group.entry_local_order_id == 0) {
      group.entry_local_order_id = local_order_id;
    }
    return true;
  }

  [[nodiscard]] bool StartCloseOrder(
      ExecutionGroup& group, std::uint64_t local_order_id,
      CloseOrderKind close_order_kind = CloseOrderKind::kNormal) noexcept {
    if (local_order_id == 0) {
      return false;
    }
    if (close_order_kind == CloseOrderKind::kNone) {
      return false;
    }
    if (group.hold() && !group.pending_order()) {
      group.stage = ExecutionStage::kClose;
      group.close_order_kind = close_order_kind;
      return AddPendingOrder(group, local_order_id);
    }
    if (group.stage == ExecutionStage::kClose &&
        group.close_order_kind == close_order_kind) {
      return AddPendingOrder(group, local_order_id);
    }
    return false;
  }

  [[nodiscard]] ExecutionGroup* AddHoldGroup(double signed_position_quantity,
                                             double trailing_price) noexcept {
    ExecutionGroup* group = FindIdleGroup();
    if (group == nullptr) {
      return nullptr;
    }
    *group = ExecutionGroup{
        .stage = ExecutionStage::kHold,
        .signed_position_quantity = signed_position_quantity,
        .trailing_price = trailing_price,
        .group_id = next_group_id_++,
    };
    ++active_group_count_;
    return group;
  }

  [[nodiscard]] ExecutionApplyResult ApplyTerminalOrder(
      const core::StrategyOrder& order,
      const InstrumentMetadata& instrument) noexcept {
    if (!order.is_finished) {
      return ExecutionApplyResult::kIgnoredNonTerminal;
    }
    ExecutionGroup* group =
        FindPendingOrderByLocalOrderId(order.local_order_id);
    if (group == nullptr) {
      return ExecutionApplyResult::kIgnoredUnknownOrder;
    }

    const ExecutionStage previous_stage = group->stage;
    const CloseOrderKind previous_close_order_kind = group->close_order_kind;
    const bool resolved_unknown_result =
        ClearUnknownResultPending(*group, order.local_order_id);
    [[maybe_unused]] const bool removed =
        RemovePendingOrder(*group, order.local_order_id);
    const double signed_filled_quantity =
        SignedFilledQuantity(order, instrument);
    if (previous_stage == ExecutionStage::kOpen &&
        order.AverageFillPrice() > 0.0 &&
        std::abs(signed_filled_quantity) > kExecutionQuantityEpsilon) {
      group->absolute_entry_value +=
          std::abs(signed_filled_quantity) * order.AverageFillPrice();
    }
    group->signed_position_quantity += signed_filled_quantity;

    if (previous_stage == ExecutionStage::kOpen) {
      const double absolute_position_quantity =
          std::abs(group->signed_position_quantity);
      if (absolute_position_quantity > kExecutionQuantityEpsilon &&
          group->absolute_entry_value > 0.0) {
        group->trailing_price =
            group->absolute_entry_value / absolute_position_quantity;
      }
    }
    if (group->pending_order()) {
      MaybeAutoRecoverUnknownResult(resolved_unknown_result);
      return ExecutionApplyResult::kAppliedHold;
    }
    group->close_order_kind = CloseOrderKind::kNone;

    if (std::abs(group->signed_position_quantity) <=
        kExecutionQuantityEpsilon) {
      ClearGroup(*group);
      MaybeAutoRecoverUnknownResult(resolved_unknown_result);
      return ExecutionApplyResult::kAppliedDeleted;
    }

    if (previous_stage == ExecutionStage::kClose &&
        previous_close_order_kind == CloseOrderKind::kNormal) {
      IncrementNormalCloseRetryCount(*group);
    }
    group->stage = ExecutionStage::kHold;
    MaybeAutoRecoverUnknownResult(resolved_unknown_result);
    return ExecutionApplyResult::kAppliedHold;
  }

  [[nodiscard]] ExecutionApplyResult ApplySubmitRejected(
      std::uint64_t local_order_id) noexcept {
    ExecutionGroup* group = FindPendingOrderByLocalOrderId(local_order_id);
    if (group == nullptr) {
      return ExecutionApplyResult::kIgnoredUnknownOrder;
    }
    const CloseOrderKind previous_close_order_kind = group->close_order_kind;
    [[maybe_unused]] const bool removed =
        RemovePendingOrder(*group, local_order_id);
    if (group->pending_order()) {
      return ExecutionApplyResult::kAppliedHold;
    }
    group->close_order_kind = CloseOrderKind::kNone;
    if (std::abs(group->signed_position_quantity) <=
        kExecutionQuantityEpsilon) {
      ClearGroup(*group);
      return ExecutionApplyResult::kAppliedDeleted;
    }
    if (group->stage == ExecutionStage::kClose &&
        previous_close_order_kind == CloseOrderKind::kNormal) {
      IncrementNormalCloseRetryCount(*group);
    }
    group->stage = ExecutionStage::kHold;
    return ExecutionApplyResult::kAppliedHold;
  }

  void OnFeedbackContinuityLost(const OrderFeedbackEvent&) noexcept {
    MarkNeedsReconcile();
  }

  void MarkNeedsReconcile() noexcept {
    if (recovery_state_ != RecoveryState::kManualIntervention) {
      recovery_state_ = RecoveryState::kDegradedNeedsReconcile;
    }
    needs_reconcile_ = true;
    unknown_result_reconcile_active_ = false;
  }

  [[nodiscard]] bool MarkUnknownResult(std::uint64_t local_order_id) noexcept {
    ExecutionGroup* group = FindPendingOrderByLocalOrderId(local_order_id);
    if (group == nullptr) {
      return false;
    }
    const bool can_auto_recover =
        !needs_reconcile_ || unknown_result_reconcile_active_;
    const bool newly_marked = MarkGroupUnknownResult(*group, local_order_id);
    if (newly_marked) {
      ++unknown_result_pending_count_;
    }
    if (recovery_state_ != RecoveryState::kManualIntervention) {
      recovery_state_ = RecoveryState::kDegradedNeedsReconcile;
      unknown_result_reconcile_active_ = can_auto_recover;
    }
    needs_reconcile_ = true;
    return newly_marked;
  }

  [[nodiscard]] bool BeginReconcile() noexcept {
    if (recovery_state_ == RecoveryState::kReconciling) {
      return true;
    }
    if (recovery_state_ != RecoveryState::kDegradedNeedsReconcile) {
      return false;
    }
    recovery_state_ = RecoveryState::kReconciling;
    needs_reconcile_ = true;
    unknown_result_reconcile_active_ = false;
    return true;
  }

  [[nodiscard]] bool ApplyRecoveryResult(
      const RecoveryApplyResult& result) noexcept {
    if (!RecoveryApplySucceeded(result)) {
      recovery_state_ = RecoveryState::kManualIntervention;
      needs_reconcile_ = true;
      unknown_result_reconcile_active_ = false;
      return false;
    }
    if (recovery_state_ != RecoveryState::kReconciling) {
      MarkNeedsReconcile();
      return false;
    }
    recovery_state_ = RecoveryState::kNormal;
    needs_reconcile_ = false;
    ClearUnknownResultTracking();
    return true;
  }

  [[nodiscard]] bool ConsumeUnknownResultAutoRecovered() noexcept {
    const bool recovered = last_unknown_result_auto_recovered_;
    last_unknown_result_auto_recovered_ = false;
    return recovered;
  }

  [[nodiscard]] const ExecutionGroup* FindGroupById(
      std::uint64_t group_id) const noexcept {
    for (const ExecutionGroup& group : groups_) {
      if (group.active() && group.group_id == group_id) {
        return &group;
      }
    }
    return nullptr;
  }

  [[nodiscard]] ExecutionGroup* FindGroupById(std::uint64_t group_id) noexcept {
    for (ExecutionGroup& group : groups_) {
      if (group.active() && group.group_id == group_id) {
        return &group;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool ClearGroupById(std::uint64_t group_id) noexcept {
    ExecutionGroup* group = FindGroupById(group_id);
    if (group == nullptr) {
      return false;
    }
    ClearGroup(*group);
    return true;
  }

  [[nodiscard]] const ExecutionGroup* FindPendingOrderByLocalOrderId(
      std::uint64_t local_order_id) const noexcept {
    // execute.parallel is a small bounded risk limit, so scanning contiguous
    // groups avoids maintaining a second order index.
    for (const ExecutionGroup& group : groups_) {
      if (ContainsPendingOrder(group, local_order_id)) {
        return &group;
      }
    }
    return nullptr;
  }

  [[nodiscard]] ExecutionGroup* FindPendingOrderByLocalOrderId(
      std::uint64_t local_order_id) noexcept {
    for (ExecutionGroup& group : groups_) {
      if (ContainsPendingOrder(group, local_order_id)) {
        return &group;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::size_t active_group_count() const noexcept {
    return active_group_count_;
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return groups_.size();
  }

  [[nodiscard]] const std::vector<ExecutionGroup>& groups() const noexcept {
    return groups_;
  }

  [[nodiscard]] bool degraded() const noexcept {
    return RecoveryStatePausesNewEntries(recovery_state_);
  }

  [[nodiscard]] bool needs_reconcile() const noexcept {
    return needs_reconcile_;
  }

  [[nodiscard]] RecoveryState recovery_state() const noexcept {
    return recovery_state_;
  }

  [[nodiscard]] bool manual_intervention() const noexcept {
    return RecoveryStateManualIntervention(recovery_state_);
  }

  [[nodiscard]] bool new_entries_paused() const noexcept {
    return RecoveryStatePausesNewEntries(recovery_state_) || needs_reconcile_;
  }

 private:
  friend class SignalEngine;

  [[nodiscard]] static double SignedFilledQuantity(
      const core::StrategyOrder& order,
      const InstrumentMetadata& instrument) noexcept {
    const double filled =
        NormalizeFilledQuantity(order.cumulative_filled_quantity, instrument);
    return order.side == OrderSide::kBuy ? filled : -filled;
  }

  [[nodiscard]] static double NormalizeFilledQuantity(
      double quantity, const InstrumentMetadata& instrument) noexcept {
    if (!std::isfinite(quantity) || quantity <= 0.0) {
      return 0.0;
    }
    assert(std::isfinite(instrument.quantity_step));
    assert(instrument.quantity_step > 0.0);
    const double rounded = std::round(quantity / instrument.quantity_step) *
                           instrument.quantity_step;
    if (!std::isfinite(rounded) || rounded <= kExecutionQuantityEpsilon) {
      return 0.0;
    }
    return rounded;
  }

  [[nodiscard]] ExecutionGroup* FindIdleGroup() noexcept {
    for (ExecutionGroup& group : groups_) {
      if (!group.active()) {
        return &group;
      }
    }
    return nullptr;
  }

  void ClearGroup(ExecutionGroup& group) noexcept {
    ClearUnknownResultsForGroup(group);
    if (group.active() && active_group_count_ > 0) {
      --active_group_count_;
    }
    group = ExecutionGroup{};
  }

  [[nodiscard]] static bool AddPendingOrder(
      ExecutionGroup& group, std::uint64_t local_order_id) noexcept {
    if (group.pending_order_count >= group.pending_local_order_ids.size()) {
      return false;
    }
    group.pending_local_order_ids[group.pending_order_count++] = local_order_id;
    RefreshPrimaryPendingOrder(group);
    return true;
  }

  [[nodiscard]] static bool RemovePendingOrder(
      ExecutionGroup& group, std::uint64_t local_order_id) noexcept {
    for (std::uint8_t index = 0; index < group.pending_order_count; ++index) {
      if (group.pending_local_order_ids[index] != local_order_id) {
        continue;
      }
      const std::uint8_t last_index =
          static_cast<std::uint8_t>(group.pending_order_count - 1U);
      group.pending_local_order_ids[index] =
          group.pending_local_order_ids[last_index];
      group.pending_local_order_ids[last_index] = 0;
      --group.pending_order_count;
      RefreshPrimaryPendingOrder(group);
      return true;
    }
    return false;
  }

  [[nodiscard]] static bool ContainsPendingOrder(
      const ExecutionGroup& group, std::uint64_t local_order_id) noexcept {
    if (local_order_id == 0) {
      return false;
    }
    for (std::uint8_t index = 0; index < group.pending_order_count; ++index) {
      if (group.pending_local_order_ids[index] == local_order_id) {
        return true;
      }
    }
    return false;
  }

  static void RefreshPrimaryPendingOrder(ExecutionGroup& group) noexcept {
    group.local_order_id =
        group.pending_order_count == 0 ? 0 : group.pending_local_order_ids[0];
  }

  [[nodiscard]] static bool ContainsUnknownResult(
      const ExecutionGroup& group, std::uint64_t local_order_id) noexcept {
    if (local_order_id == 0) {
      return false;
    }
    for (std::uint8_t index = 0; index < group.unknown_result_pending_count;
         ++index) {
      if (group.unknown_result_local_order_ids[index] == local_order_id) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] static bool MarkGroupUnknownResult(
      ExecutionGroup& group, std::uint64_t local_order_id) noexcept {
    if (ContainsUnknownResult(group, local_order_id)) {
      return false;
    }
    if (group.unknown_result_pending_count >=
        group.unknown_result_local_order_ids.size()) {
      return false;
    }
    group.unknown_result_local_order_ids[group.unknown_result_pending_count++] =
        local_order_id;
    return true;
  }

  [[nodiscard]] bool ClearUnknownResultPending(
      ExecutionGroup& group, std::uint64_t local_order_id) noexcept {
    for (std::uint8_t index = 0; index < group.unknown_result_pending_count;
         ++index) {
      if (group.unknown_result_local_order_ids[index] != local_order_id) {
        continue;
      }
      const std::uint8_t last_index =
          static_cast<std::uint8_t>(group.unknown_result_pending_count - 1U);
      group.unknown_result_local_order_ids[index] =
          group.unknown_result_local_order_ids[last_index];
      group.unknown_result_local_order_ids[last_index] = 0;
      --group.unknown_result_pending_count;
      assert(unknown_result_pending_count_ > 0);
      if (unknown_result_pending_count_ > 0) {
        --unknown_result_pending_count_;
      }
      return true;
    }
    return false;
  }

  void ClearUnknownResultsForGroup(ExecutionGroup& group) noexcept {
    if (group.unknown_result_pending_count == 0) {
      return;
    }
    assert(unknown_result_pending_count_ > 0);
    if (unknown_result_pending_count_ >= group.unknown_result_pending_count) {
      unknown_result_pending_count_ -= group.unknown_result_pending_count;
    } else {
      unknown_result_pending_count_ = 0;
    }
    for (std::uint8_t index = 0; index < group.unknown_result_pending_count;
         ++index) {
      group.unknown_result_local_order_ids[index] = 0;
    }
    group.unknown_result_pending_count = 0;
  }

  void MaybeAutoRecoverUnknownResult(bool resolved_unknown_result) noexcept {
    if (!resolved_unknown_result || unknown_result_pending_count_ != 0 ||
        !unknown_result_reconcile_active_ ||
        recovery_state_ != RecoveryState::kDegradedNeedsReconcile) {
      return;
    }
    recovery_state_ = RecoveryState::kNormal;
    needs_reconcile_ = false;
    unknown_result_reconcile_active_ = false;
    last_unknown_result_auto_recovered_ = true;
  }

  static void IncrementNormalCloseRetryCount(ExecutionGroup& group) noexcept {
    if (group.normal_close_retry_count <
        std::numeric_limits<std::uint32_t>::max()) {
      ++group.normal_close_retry_count;
    }
  }

  void ClearUnknownResultTracking() noexcept {
    for (ExecutionGroup& group : groups_) {
      for (std::uint64_t& local_order_id :
           group.unknown_result_local_order_ids) {
        local_order_id = 0;
      }
      group.unknown_result_pending_count = 0;
    }
    unknown_result_pending_count_ = 0;
    unknown_result_reconcile_active_ = false;
    last_unknown_result_auto_recovered_ = false;
  }

  [[nodiscard]] std::vector<ExecutionGroup>& mutable_groups() noexcept {
    return groups_;
  }

  std::vector<ExecutionGroup> groups_;
  std::size_t active_group_count_{0};
  std::uint64_t next_group_id_{1};
  RecoveryState recovery_state_{RecoveryState::kNormal};
  bool needs_reconcile_{false};
  std::size_t unknown_result_pending_count_{0};
  bool unknown_result_reconcile_active_{false};
  bool last_unknown_result_auto_recovered_{false};
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_EXECUTION_STATE_H_
