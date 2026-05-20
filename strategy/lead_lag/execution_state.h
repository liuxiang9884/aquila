#ifndef AQUILA_STRATEGY_LEAD_LAG_EXECUTION_STATE_H_
#define AQUILA_STRATEGY_LEAD_LAG_EXECUTION_STATE_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/strategy/order_types.h"
#include "core/trading/order_feedback_event.h"
#include "strategy/lead_lag/config.h"

namespace aquila::strategy::leadlag {

enum class ExecutionStage : std::uint8_t {
  kIdle,
  kOpen,
  kHold,
  kClose,
};

enum class ExecutionApplyResult : std::uint8_t {
  kIgnoredUnknownOrder,
  kIgnoredNonTerminal,
  kAppliedHold,
  kAppliedDeleted,
};

class SignalEngine;

struct ExecutionGroup {
  ExecutionStage stage{ExecutionStage::kIdle};
  std::uint64_t local_order_id{0};
  std::int64_t signed_position_quantity{0};
  double trailing_price{0.0};
  std::uint64_t group_id{0};

  [[nodiscard]] bool active() const noexcept {
    return stage != ExecutionStage::kIdle;
  }

  [[nodiscard]] bool hold() const noexcept {
    return stage == ExecutionStage::kHold;
  }

  [[nodiscard]] bool pending_order() const noexcept {
    return local_order_id != 0;
  }

  [[nodiscard]] bool long_position() const noexcept {
    return signed_position_quantity > 0;
  }

  [[nodiscard]] bool short_position() const noexcept {
    return signed_position_quantity < 0;
  }
};

class ExecutionState {
 public:
  void Init(std::uint32_t parallel) {
    groups_.assign(parallel, ExecutionGroup{});
    active_group_count_ = 0;
    next_group_id_ = 1;
    degraded_ = false;
    needs_reconcile_ = false;
  }

  [[nodiscard]] ExecutionGroup* StartOpenOrder(
      std::uint64_t local_order_id) noexcept {
    ExecutionGroup* group = FindIdleGroup();
    if (group == nullptr) {
      return nullptr;
    }
    *group = ExecutionGroup{
        .stage = ExecutionStage::kOpen,
        .local_order_id = local_order_id,
        .group_id = next_group_id_++,
    };
    ++active_group_count_;
    return group;
  }

  [[nodiscard]] bool StartCloseOrder(ExecutionGroup& group,
                                     std::uint64_t local_order_id) noexcept {
    if (!group.hold() || group.pending_order()) {
      return false;
    }
    group.stage = ExecutionStage::kClose;
    group.local_order_id = local_order_id;
    return true;
  }

  [[nodiscard]] ExecutionGroup* AddHoldGroup(
      std::int64_t signed_position_quantity, double trailing_price) noexcept {
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
      const strategy::StrategyOrder& order,
      const InstrumentMetadata& instrument) noexcept {
    if (!order.is_finished) {
      return ExecutionApplyResult::kIgnoredNonTerminal;
    }
    ExecutionGroup* group = FindPendingOrder(order.local_order_id);
    if (group == nullptr) {
      return ExecutionApplyResult::kIgnoredUnknownOrder;
    }

    const ExecutionStage previous_stage = group->stage;
    group->local_order_id = 0;
    group->signed_position_quantity += SignedFilledQuantity(order, instrument);

    if (group->signed_position_quantity == 0) {
      ClearGroup(*group);
      return ExecutionApplyResult::kAppliedDeleted;
    }

    if (previous_stage == ExecutionStage::kOpen &&
        order.AverageFillPrice() > 0.0) {
      group->trailing_price = order.AverageFillPrice();
    }
    group->stage = ExecutionStage::kHold;
    return ExecutionApplyResult::kAppliedHold;
  }

  [[nodiscard]] ExecutionApplyResult ApplySubmitRejected(
      std::uint64_t local_order_id) noexcept {
    ExecutionGroup* group = FindPendingOrder(local_order_id);
    if (group == nullptr) {
      return ExecutionApplyResult::kIgnoredUnknownOrder;
    }
    group->local_order_id = 0;
    if (group->signed_position_quantity == 0) {
      ClearGroup(*group);
      return ExecutionApplyResult::kAppliedDeleted;
    }
    group->stage = ExecutionStage::kHold;
    return ExecutionApplyResult::kAppliedHold;
  }

  void OnFeedbackContinuityLost(const OrderFeedbackEvent&) noexcept {
    degraded_ = true;
    needs_reconcile_ = true;
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
    return degraded_;
  }

  [[nodiscard]] bool needs_reconcile() const noexcept {
    return needs_reconcile_;
  }

  [[nodiscard]] bool new_entries_paused() const noexcept {
    return degraded_ || needs_reconcile_;
  }

 private:
  friend class SignalEngine;

  [[nodiscard]] static std::int64_t SignedFilledQuantity(
      const strategy::StrategyOrder& order,
      const InstrumentMetadata& instrument) noexcept {
    const std::int64_t filled =
        NormalizeFilledQuantity(order.cumulative_filled_quantity, instrument);
    return order.side == OrderSide::kBuy ? filled : -filled;
  }

  [[nodiscard]] static std::int64_t NormalizeFilledQuantity(
      std::int64_t quantity, const InstrumentMetadata& instrument) noexcept {
    const double unit =
        instrument.quantity_step * instrument.notional_multiplier;
    if (unit <= 0.0) {
      return quantity;
    }
    return static_cast<std::int64_t>(
        std::llround(std::round(static_cast<double>(quantity) / unit) * unit));
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
    if (group.active() && active_group_count_ > 0) {
      --active_group_count_;
    }
    group = ExecutionGroup{};
  }

  [[nodiscard]] std::vector<ExecutionGroup>& mutable_groups() noexcept {
    return groups_;
  }

  [[nodiscard]] ExecutionGroup* FindPendingOrder(
      std::uint64_t local_order_id) noexcept {
    // execute.parallel is a small bounded risk limit, so scanning contiguous
    // groups avoids maintaining a second order index.
    for (ExecutionGroup& group : groups_) {
      if (group.pending_order() && group.local_order_id == local_order_id) {
        return &group;
      }
    }
    return nullptr;
  }

  std::vector<ExecutionGroup> groups_;
  std::size_t active_group_count_{0};
  std::uint64_t next_group_id_{1};
  bool degraded_{false};
  bool needs_reconcile_{false};
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_EXECUTION_STATE_H_
