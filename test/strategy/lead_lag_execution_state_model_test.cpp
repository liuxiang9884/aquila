#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "strategy/lead_lag/execution_state.h"

namespace {

namespace leadlag = aquila::strategy::leadlag;

constexpr std::size_t kModelCapacity = leadlag::kMaxLeadLagExecutionGroups;
constexpr double kTolerance = 1e-9;

struct ModelPendingOrder {
  std::uint64_t local_order_id{0};
  leadlag::PendingOrderRole role{leadlag::PendingOrderRole::kNone};
  aquila::OrderSide side{aquila::OrderSide::kBuy};
};

struct ModelGroup {
  bool active{false};
  leadlag::ExecutionGroupIndex group_index{
      leadlag::kInvalidExecutionGroupIndex};
  std::uint64_t group_id{0};
  std::uint64_t entry_local_order_id{0};
  leadlag::ExecutionStage stage{leadlag::ExecutionStage::kIdle};
  std::vector<ModelPendingOrder> pending;
  std::vector<std::uint64_t> unknown_result_order_ids;
  double signed_position_quantity{0.0};
  double absolute_entry_value{0.0};
  double trailing_price{0.0};
  leadlag::CloseOrderKind close_order_kind{leadlag::CloseOrderKind::kNone};
  std::uint32_t normal_close_retry_count{0};
  double close_base_position_quantity{0.0};
  double close_filled_quantity{0.0};
};

struct ReferenceModel {
  std::array<ModelGroup, kModelCapacity> groups{};
  std::vector<leadlag::ExecutionGroupIndex> active_indices;
  std::size_t capacity{0};
  std::uint64_t next_group_id{1};
  std::size_t unknown_result_count{0};
  leadlag::RecoveryState recovery_state{leadlag::RecoveryState::kNormal};
  bool needs_reconcile{false};
  bool unknown_result_reconcile_active{false};
  bool last_unknown_result_auto_recovered{false};

  [[nodiscard]] bool NewEntriesPaused() const {
    return recovery_state == leadlag::RecoveryState::kDegradedNeedsReconcile ||
           recovery_state == leadlag::RecoveryState::kReconciling ||
           recovery_state == leadlag::RecoveryState::kManualIntervention ||
           needs_reconcile;
  }
};

struct RetiredOrder {
  aquila::core::StrategyOrder order;
};

[[nodiscard]] leadlag::InstrumentMetadata Instrument() {
  return leadlag::InstrumentMetadata{
      .quantity_step = 1.0,
      .notional_multiplier = 1.0,
  };
}

[[nodiscard]] bool IsCloseRole(leadlag::PendingOrderRole role) {
  return role == leadlag::PendingOrderRole::kNormalClose ||
         role == leadlag::PendingOrderRole::kStoplossClose;
}

[[nodiscard]] bool IsNormalCloseRole(leadlag::PendingOrderRole role) {
  return role == leadlag::PendingOrderRole::kNormalClose;
}

[[nodiscard]] std::size_t PendingRoleCount(const ModelGroup& group,
                                           leadlag::PendingOrderRole role) {
  return static_cast<std::size_t>(std::count_if(
      group.pending.begin(), group.pending.end(),
      [role](const ModelPendingOrder& order) { return order.role == role; }));
}

[[nodiscard]] leadlag::ExecutionGroupIndex FindFreeIndex(
    const ReferenceModel& model) {
  for (std::size_t index = 0; index < model.capacity; ++index) {
    if (!model.groups[index].active) {
      return static_cast<leadlag::ExecutionGroupIndex>(index);
    }
  }
  return leadlag::kInvalidExecutionGroupIndex;
}

ModelGroup* AddModelGroup(ReferenceModel* model, leadlag::ExecutionStage stage,
                          double signed_position_quantity,
                          double trailing_price) {
  const leadlag::ExecutionGroupIndex index = FindFreeIndex(*model);
  if (index == leadlag::kInvalidExecutionGroupIndex) {
    return nullptr;
  }
  ModelGroup& group = model->groups[index];
  group = ModelGroup{
      .active = true,
      .group_index = index,
      .group_id = model->next_group_id++,
      .stage = stage,
      .signed_position_quantity = signed_position_quantity,
      .trailing_price = trailing_price,
  };
  model->active_indices.push_back(index);
  return &group;
}

void ClearUnknownResultsForGroup(ReferenceModel* model, ModelGroup* group) {
  ASSERT_LE(group->unknown_result_order_ids.size(),
            model->unknown_result_count);
  model->unknown_result_count -= group->unknown_result_order_ids.size();
  group->unknown_result_order_ids.clear();
}

void RemoveModelGroup(ReferenceModel* model, ModelGroup* group) {
  const leadlag::ExecutionGroupIndex index = group->group_index;
  ClearUnknownResultsForGroup(model, group);
  model->active_indices.erase(std::find(model->active_indices.begin(),
                                        model->active_indices.end(), index));
  model->groups[index] = ModelGroup{};
}

void AddPendingOrder(ModelGroup* group, std::uint64_t local_order_id,
                     leadlag::PendingOrderRole role, aquila::OrderSide side) {
  group->pending.push_back(ModelPendingOrder{
      .local_order_id = local_order_id,
      .role = role,
      .side = side,
  });
  if (role == leadlag::PendingOrderRole::kOpen &&
      group->entry_local_order_id == 0) {
    group->entry_local_order_id = local_order_id;
  }
}

[[nodiscard]] std::size_t FindPendingIndex(const ModelGroup& group,
                                           std::uint64_t local_order_id) {
  for (std::size_t index = 0; index < group.pending.size(); ++index) {
    if (group.pending[index].local_order_id == local_order_id) {
      return index;
    }
  }
  return group.pending.size();
}

[[nodiscard]] ModelPendingOrder RemovePendingOrder(ModelGroup* group,
                                                   std::size_t index) {
  const ModelPendingOrder removed = group->pending[index];
  group->pending[index] = group->pending.back();
  group->pending.pop_back();
  return removed;
}

void RemoveUnknownResult(ReferenceModel* model, ModelGroup* group,
                         std::uint64_t local_order_id, bool* resolved) {
  *resolved = false;
  for (std::size_t index = 0; index < group->unknown_result_order_ids.size();
       ++index) {
    if (group->unknown_result_order_ids[index] != local_order_id) {
      continue;
    }
    group->unknown_result_order_ids[index] =
        group->unknown_result_order_ids.back();
    group->unknown_result_order_ids.pop_back();
    ASSERT_GT(model->unknown_result_count, 0U);
    --model->unknown_result_count;
    *resolved = true;
    return;
  }
}

void NormalizePosition(ModelGroup* group) {
  if (std::abs(group->signed_position_quantity) <=
      leadlag::kExecutionQuantityEpsilon) {
    group->signed_position_quantity = 0.0;
    group->absolute_entry_value = 0.0;
    group->trailing_price = 0.0;
  }
}

void ApplyModelFill(ModelGroup* group, const ModelPendingOrder& pending,
                    double filled_quantity, double fill_price) {
  if (filled_quantity <= leadlag::kExecutionQuantityEpsilon) {
    return;
  }
  const double signed_fill = pending.side == aquila::OrderSide::kBuy
                                 ? filled_quantity
                                 : -filled_quantity;
  if (pending.role == leadlag::PendingOrderRole::kOpen) {
    if (fill_price > 0.0) {
      group->absolute_entry_value += std::abs(signed_fill) * fill_price;
    }
  } else if (IsCloseRole(pending.role)) {
    group->close_filled_quantity += std::abs(signed_fill);
    const double previous_position = std::abs(group->signed_position_quantity);
    if (previous_position > leadlag::kExecutionQuantityEpsilon &&
        group->absolute_entry_value > 0.0 &&
        group->signed_position_quantity * signed_fill < 0.0) {
      const double closed_quantity =
          std::min(std::abs(signed_fill), previous_position);
      group->absolute_entry_value -=
          group->absolute_entry_value / previous_position * closed_quantity;
      if (group->absolute_entry_value <= leadlag::kExecutionQuantityEpsilon) {
        group->absolute_entry_value = 0.0;
      }
    }
  }
  group->signed_position_quantity += signed_fill;
  NormalizePosition(group);
  if (pending.role != leadlag::PendingOrderRole::kOpen ||
      group->absolute_entry_value <= 0.0) {
    return;
  }
  const double average_entry =
      group->absolute_entry_value / std::abs(group->signed_position_quantity);
  if (group->trailing_price <= leadlag::kExecutionQuantityEpsilon) {
    group->trailing_price = average_entry;
  } else if (group->signed_position_quantity > 0.0) {
    group->trailing_price = std::max(group->trailing_price, average_entry);
  } else {
    group->trailing_price = std::min(group->trailing_price, average_entry);
  }
}

void FinalizeModelCloseBatch(ModelGroup* group,
                             leadlag::PendingOrderRole removed_role) {
  if (!IsCloseRole(removed_role) ||
      PendingRoleCount(*group, leadlag::PendingOrderRole::kNormalClose) +
              PendingRoleCount(*group,
                               leadlag::PendingOrderRole::kStoplossClose) !=
          0) {
    return;
  }
  group->close_order_kind = leadlag::CloseOrderKind::kNone;
  const double covered = std::min(group->close_filled_quantity,
                                  group->close_base_position_quantity);
  const bool uncovered = group->close_base_position_quantity - covered >
                         leadlag::kExecutionQuantityEpsilon;
  group->close_base_position_quantity = 0.0;
  group->close_filled_quantity = 0.0;
  if (IsNormalCloseRole(removed_role) && uncovered &&
      std::abs(group->signed_position_quantity) >
          leadlag::kExecutionQuantityEpsilon &&
      group->normal_close_retry_count <
          std::numeric_limits<std::uint32_t>::max()) {
    ++group->normal_close_retry_count;
  }
}

void RefreshModelStage(ModelGroup* group) {
  if (PendingRoleCount(*group, leadlag::PendingOrderRole::kNormalClose) +
          PendingRoleCount(*group, leadlag::PendingOrderRole::kStoplossClose) !=
      0) {
    group->stage = leadlag::ExecutionStage::kClose;
  } else if (PendingRoleCount(*group, leadlag::PendingOrderRole::kOpen) != 0) {
    group->stage = leadlag::ExecutionStage::kOpen;
  } else if (std::abs(group->signed_position_quantity) >
             leadlag::kExecutionQuantityEpsilon) {
    group->stage = leadlag::ExecutionStage::kHold;
  }
}

void MaybeAutoRecover(ReferenceModel* model, bool resolved_unknown_result) {
  if (!resolved_unknown_result || model->unknown_result_count != 0 ||
      !model->unknown_result_reconcile_active ||
      model->recovery_state !=
          leadlag::RecoveryState::kDegradedNeedsReconcile) {
    return;
  }
  model->recovery_state = leadlag::RecoveryState::kNormal;
  model->needs_reconcile = false;
  model->unknown_result_reconcile_active = false;
  model->last_unknown_result_auto_recovered = true;
}

[[nodiscard]] leadlag::ExecutionApplyResult ApplyModelTerminal(
    ReferenceModel* model, const aquila::core::StrategyOrder& order) {
  if (!order.is_finished) {
    return leadlag::ExecutionApplyResult::kIgnoredNonTerminal;
  }
  if (order.group_index >= model->capacity) {
    return leadlag::ExecutionApplyResult::kIgnoredGroupMismatch;
  }
  ModelGroup& group = model->groups[order.group_index];
  if (!group.active || group.group_id != order.place_request.group_id) {
    return leadlag::ExecutionApplyResult::kIgnoredGroupMismatch;
  }
  const std::size_t pending_index =
      FindPendingIndex(group, order.place_request.local_order_id);
  if (pending_index == group.pending.size()) {
    return leadlag::ExecutionApplyResult::kIgnoredUnknownOrder;
  }
  bool resolved_unknown_result = false;
  RemoveUnknownResult(model, &group, order.place_request.local_order_id,
                      &resolved_unknown_result);
  const ModelPendingOrder pending = RemovePendingOrder(&group, pending_index);
  ApplyModelFill(&group, pending, order.cumulative_filled_quantity,
                 order.AverageFillPrice());
  FinalizeModelCloseBatch(&group, pending.role);
  if (!group.pending.empty()) {
    RefreshModelStage(&group);
    MaybeAutoRecover(model, resolved_unknown_result);
    return leadlag::ExecutionApplyResult::kAppliedHold;
  }
  if (std::abs(group.signed_position_quantity) <=
      leadlag::kExecutionQuantityEpsilon) {
    RemoveModelGroup(model, &group);
    MaybeAutoRecover(model, resolved_unknown_result);
    return leadlag::ExecutionApplyResult::kAppliedDeleted;
  }
  group.stage = leadlag::ExecutionStage::kHold;
  MaybeAutoRecover(model, resolved_unknown_result);
  return leadlag::ExecutionApplyResult::kAppliedHold;
}

[[nodiscard]] leadlag::ExecutionApplyResult ApplyModelRejected(
    ReferenceModel* model, const aquila::core::StrategyOrder& order) {
  if (order.group_index >= model->capacity) {
    return leadlag::ExecutionApplyResult::kIgnoredGroupMismatch;
  }
  ModelGroup& group = model->groups[order.group_index];
  if (!group.active || group.group_id != order.place_request.group_id) {
    return leadlag::ExecutionApplyResult::kIgnoredGroupMismatch;
  }
  const std::size_t pending_index =
      FindPendingIndex(group, order.place_request.local_order_id);
  if (pending_index == group.pending.size()) {
    return leadlag::ExecutionApplyResult::kIgnoredUnknownOrder;
  }
  const ModelPendingOrder pending = RemovePendingOrder(&group, pending_index);
  FinalizeModelCloseBatch(&group, pending.role);
  if (!group.pending.empty()) {
    RefreshModelStage(&group);
    return leadlag::ExecutionApplyResult::kAppliedHold;
  }
  if (std::abs(group.signed_position_quantity) <=
      leadlag::kExecutionQuantityEpsilon) {
    RemoveModelGroup(model, &group);
    return leadlag::ExecutionApplyResult::kAppliedDeleted;
  }
  group.stage = leadlag::ExecutionStage::kHold;
  return leadlag::ExecutionApplyResult::kAppliedHold;
}

[[nodiscard]] aquila::core::StrategyOrder MakeOrder(
    const ModelGroup& group, const ModelPendingOrder& pending,
    double filled_quantity, double fill_price, bool is_finished = true) {
  return aquila::core::StrategyOrder{
      .place_request =
          aquila::core::OrderPlaceRequest{
              .local_order_id = pending.local_order_id,
              .group_id = group.group_id,
              .quantity = filled_quantity,
              .side = pending.side,
              .reduce_only = IsCloseRole(pending.role),
          },
      .group_index = group.group_index,
      .status = filled_quantity > 0.0 ? aquila::core::OrderStatus::kFilled
                                      : aquila::core::OrderStatus::kCancelled,
      .cumulative_filled_quantity = filled_quantity,
      .cumulative_filled_value = filled_quantity * fill_price,
      .last_fill_price = fill_price,
      .is_finished = is_finished,
  };
}

void CompareState(const leadlag::ExecutionState& actual,
                  const ReferenceModel& expected) {
  EXPECT_EQ(actual.capacity(), expected.capacity);
  EXPECT_EQ(actual.active_group_count(), expected.active_indices.size());
  EXPECT_LE(actual.active_group_count(), actual.capacity());
  EXPECT_EQ(actual.recovery_state(), expected.recovery_state);
  EXPECT_EQ(actual.needs_reconcile(), expected.needs_reconcile);
  EXPECT_EQ(actual.new_entries_paused(), expected.NewEntriesPaused());

  std::vector<const leadlag::ExecutionGroup*> actual_groups;
  actual.ForEachActiveGroup(
      [&actual_groups](const leadlag::ExecutionGroup& group) {
        actual_groups.push_back(&group);
      });
  ASSERT_EQ(actual_groups.size(), expected.active_indices.size());
  for (std::size_t position = 0; position < actual_groups.size(); ++position) {
    const leadlag::ExecutionGroup& actual_group = *actual_groups[position];
    const ModelGroup& expected_group =
        expected.groups[expected.active_indices[position]];
    EXPECT_EQ(actual_group.group_index, expected_group.group_index);
    EXPECT_EQ(actual_group.group_id, expected_group.group_id);
    EXPECT_NE(actual_group.group_id, 0U);
    EXPECT_EQ(actual_group.entry_local_order_id,
              expected_group.entry_local_order_id);
    EXPECT_EQ(actual_group.stage, expected_group.stage);
    EXPECT_NEAR(actual_group.signed_position_quantity,
                expected_group.signed_position_quantity, kTolerance);
    EXPECT_NEAR(actual_group.absolute_entry_value,
                expected_group.absolute_entry_value, kTolerance);
    EXPECT_NEAR(actual_group.trailing_price, expected_group.trailing_price,
                kTolerance);
    EXPECT_EQ(actual_group.pending_order_count, expected_group.pending.size());
    EXPECT_EQ(actual_group.local_order_id,
              expected_group.pending.empty()
                  ? 0U
                  : expected_group.pending.front().local_order_id);
    EXPECT_EQ(
        actual_group.pending_open_order_count,
        PendingRoleCount(expected_group, leadlag::PendingOrderRole::kOpen));
    EXPECT_EQ(actual_group.pending_close_order_count,
              PendingRoleCount(expected_group,
                               leadlag::PendingOrderRole::kNormalClose) +
                  PendingRoleCount(expected_group,
                                   leadlag::PendingOrderRole::kStoplossClose));
    EXPECT_EQ(actual_group.unknown_result_pending_count,
              expected_group.unknown_result_order_ids.size());
    EXPECT_EQ(actual_group.close_order_kind, expected_group.close_order_kind);
    EXPECT_EQ(actual_group.normal_close_retry_count,
              expected_group.normal_close_retry_count);
    EXPECT_NEAR(actual_group.close_base_position_quantity,
                expected_group.close_base_position_quantity, kTolerance);
    EXPECT_NEAR(actual_group.close_filled_quantity,
                expected_group.close_filled_quantity, kTolerance);
    EXPECT_EQ(
        actual.GroupAt(expected_group.group_index, expected_group.group_id),
        &actual_group);
    EXPECT_EQ(
        actual.GroupAt(expected_group.group_index, expected_group.group_id + 1),
        nullptr);
    for (std::size_t prior = 0; prior < position; ++prior) {
      EXPECT_NE(actual_groups[prior]->group_id, actual_group.group_id);
    }
    for (std::size_t index = 0; index < expected_group.pending.size();
         ++index) {
      EXPECT_EQ(actual_group.pending_local_order_ids[index],
                expected_group.pending[index].local_order_id);
      EXPECT_EQ(actual_group.pending_order_roles[index],
                expected_group.pending[index].role);
    }
    for (std::size_t index = 0;
         index < expected_group.unknown_result_order_ids.size(); ++index) {
      EXPECT_EQ(actual_group.unknown_result_local_order_ids[index],
                expected_group.unknown_result_order_ids[index]);
    }
  }
}

[[nodiscard]] ModelGroup* RandomActiveGroup(ReferenceModel* model,
                                            std::mt19937_64* random) {
  if (model->active_indices.empty()) {
    return nullptr;
  }
  const std::size_t position =
      static_cast<std::size_t>((*random)() % model->active_indices.size());
  return &model->groups[model->active_indices[position]];
}

[[nodiscard]] ModelGroup* RandomGroupWithPending(ReferenceModel* model,
                                                 std::mt19937_64* random) {
  std::vector<leadlag::ExecutionGroupIndex> candidates;
  for (const leadlag::ExecutionGroupIndex index : model->active_indices) {
    if (!model->groups[index].pending.empty()) {
      candidates.push_back(index);
    }
  }
  if (candidates.empty()) {
    return nullptr;
  }
  return &model->groups[candidates[(*random)() % candidates.size()]];
}

[[nodiscard]] ModelGroup* RandomOpenGroup(ReferenceModel* model,
                                          std::mt19937_64* random) {
  std::vector<leadlag::ExecutionGroupIndex> candidates;
  for (const leadlag::ExecutionGroupIndex index : model->active_indices) {
    const ModelGroup& group = model->groups[index];
    if (group.stage == leadlag::ExecutionStage::kOpen &&
        group.pending.size() < leadlag::kMaxExecutionGroupPendingOrders) {
      candidates.push_back(index);
    }
  }
  if (candidates.empty()) {
    return nullptr;
  }
  return &model->groups[candidates[(*random)() % candidates.size()]];
}

void CreateOpen(leadlag::ExecutionState* actual, ReferenceModel* model,
                std::uint64_t* next_order_id, std::mt19937_64* random) {
  if (model->NewEntriesPaused()) {
    return;
  }
  const std::uint64_t local_order_id = (*next_order_id)++;
  const aquila::OrderSide side = ((*random)() & 1U) == 0U
                                     ? aquila::OrderSide::kBuy
                                     : aquila::OrderSide::kSell;
  leadlag::ExecutionGroup* actual_group =
      actual->StartOpenOrder(local_order_id);
  ModelGroup* expected_group =
      AddModelGroup(model, leadlag::ExecutionStage::kOpen, 0.0, 0.0);
  if (expected_group == nullptr) {
    EXPECT_EQ(actual_group, nullptr);
    return;
  }
  ASSERT_NE(actual_group, nullptr);
  AddPendingOrder(expected_group, local_order_id,
                  leadlag::PendingOrderRole::kOpen, side);
  EXPECT_EQ(actual_group->group_index, expected_group->group_index);
  EXPECT_EQ(actual_group->group_id, expected_group->group_id);
}

void CreateHold(leadlag::ExecutionState* actual, ReferenceModel* model,
                std::mt19937_64* random) {
  if (model->NewEntriesPaused()) {
    return;
  }
  const double quantity =
      static_cast<double>(1U + static_cast<unsigned>((*random)() % 5U));
  const double signed_quantity =
      ((*random)() & 1U) == 0U ? quantity : -quantity;
  const double trailing_price = signed_quantity > 0.0 ? 100.0 : 200.0;
  leadlag::ExecutionGroup* actual_group =
      actual->AddHoldGroup(signed_quantity, trailing_price);
  ModelGroup* expected_group = AddModelGroup(
      model, leadlag::ExecutionStage::kHold, signed_quantity, trailing_price);
  if (expected_group == nullptr) {
    EXPECT_EQ(actual_group, nullptr);
    return;
  }
  ASSERT_NE(actual_group, nullptr);
  EXPECT_EQ(actual_group->group_index, expected_group->group_index);
  EXPECT_EQ(actual_group->group_id, expected_group->group_id);
}

void AddOpenChild(leadlag::ExecutionState* actual, ReferenceModel* model,
                  std::uint64_t* next_order_id, std::mt19937_64* random) {
  ModelGroup* group = RandomOpenGroup(model, random);
  if (group == nullptr) {
    return;
  }
  const std::uint64_t local_order_id = (*next_order_id)++;
  const aquila::OrderSide side = group->signed_position_quantity < 0.0
                                     ? aquila::OrderSide::kSell
                                     : aquila::OrderSide::kBuy;
  leadlag::ExecutionGroup* actual_group =
      actual->GroupAt(group->group_index, group->group_id);
  ASSERT_NE(actual_group, nullptr);
  ASSERT_TRUE(actual->AddOpenOrder(*actual_group, local_order_id));
  AddPendingOrder(group, local_order_id, leadlag::PendingOrderRole::kOpen,
                  side);
}

void StartClose(leadlag::ExecutionState* actual, ReferenceModel* model,
                std::uint64_t* next_order_id, std::mt19937_64* random) {
  std::vector<leadlag::ExecutionGroupIndex> candidates;
  for (const leadlag::ExecutionGroupIndex index : model->active_indices) {
    const ModelGroup& group = model->groups[index];
    const std::size_t close_count =
        PendingRoleCount(group, leadlag::PendingOrderRole::kNormalClose) +
        PendingRoleCount(group, leadlag::PendingOrderRole::kStoplossClose);
    if (std::abs(group.signed_position_quantity) >
            leadlag::kExecutionQuantityEpsilon &&
        close_count < 4U &&
        (close_count == 0U || group.stage == leadlag::ExecutionStage::kClose)) {
      candidates.push_back(index);
    }
  }
  if (candidates.empty()) {
    return;
  }
  ModelGroup* group =
      &model->groups[candidates[(*random)() % candidates.size()]];
  const std::size_t existing_close_count =
      PendingRoleCount(*group, leadlag::PendingOrderRole::kNormalClose) +
      PendingRoleCount(*group, leadlag::PendingOrderRole::kStoplossClose);
  const leadlag::CloseOrderKind kind =
      existing_close_count == 0U
          ? (((*random)() & 1U) == 0U ? leadlag::CloseOrderKind::kNormal
                                      : leadlag::CloseOrderKind::kStoploss)
          : group->close_order_kind;
  const leadlag::PendingOrderRole role =
      kind == leadlag::CloseOrderKind::kNormal
          ? leadlag::PendingOrderRole::kNormalClose
          : leadlag::PendingOrderRole::kStoplossClose;
  const std::uint64_t local_order_id = (*next_order_id)++;
  leadlag::ExecutionGroup* actual_group =
      actual->GroupAt(group->group_index, group->group_id);
  ASSERT_NE(actual_group, nullptr);
  ASSERT_TRUE(actual->StartCloseOrder(*actual_group, local_order_id, kind));
  if (existing_close_count == 0U) {
    group->stage = leadlag::ExecutionStage::kClose;
    group->close_order_kind = kind;
    group->close_base_position_quantity =
        std::abs(group->signed_position_quantity);
    group->close_filled_quantity = 0.0;
  }
  AddPendingOrder(group, local_order_id, role,
                  group->signed_position_quantity > 0.0
                      ? aquila::OrderSide::kSell
                      : aquila::OrderSide::kBuy);
}

void ApplyRandomTerminal(leadlag::ExecutionState* actual, ReferenceModel* model,
                         std::vector<RetiredOrder>* retired,
                         std::mt19937_64* random) {
  ModelGroup* group = RandomGroupWithPending(model, random);
  if (group == nullptr) {
    return;
  }
  const ModelPendingOrder pending =
      group->pending[(*random)() % group->pending.size()];
  double fill = static_cast<double>((*random)() % 4U);
  if (IsCloseRole(pending.role)) {
    fill = std::min(fill, std::abs(group->signed_position_quantity));
  }
  const double price = IsCloseRole(pending.role) ? 99.0 : 100.0;
  aquila::core::StrategyOrder order = MakeOrder(*group, pending, fill, price);
  const leadlag::ExecutionApplyResult expected =
      ApplyModelTerminal(model, order);
  const leadlag::ExecutionApplyResult observed =
      actual->ApplyTerminalOrder(order, Instrument());
  EXPECT_EQ(observed, expected);
  retired->push_back(RetiredOrder{.order = order});
  const bool expected_auto_recovered =
      model->last_unknown_result_auto_recovered;
  model->last_unknown_result_auto_recovered = false;
  EXPECT_EQ(actual->ConsumeUnknownResultAutoRecovered(),
            expected_auto_recovered);
}

void ApplyRandomRejected(leadlag::ExecutionState* actual, ReferenceModel* model,
                         std::vector<RetiredOrder>* retired,
                         std::mt19937_64* random) {
  ModelGroup* group = RandomGroupWithPending(model, random);
  if (group == nullptr) {
    return;
  }
  const ModelPendingOrder pending =
      group->pending[(*random)() % group->pending.size()];
  aquila::core::StrategyOrder order = MakeOrder(*group, pending, 0.0, 0.0);
  order.status = aquila::core::OrderStatus::kRejected;
  const leadlag::ExecutionApplyResult expected =
      ApplyModelRejected(model, order);
  const leadlag::ExecutionApplyResult observed =
      actual->ApplySubmitRejected(order);
  EXPECT_EQ(observed, expected);
  retired->push_back(RetiredOrder{.order = order});
}

void MarkRandomUnknown(leadlag::ExecutionState* actual, ReferenceModel* model,
                       std::mt19937_64* random) {
  ModelGroup* group = RandomGroupWithPending(model, random);
  if (group == nullptr) {
    return;
  }
  const ModelPendingOrder pending =
      group->pending[(*random)() % group->pending.size()];
  aquila::core::StrategyOrder order =
      MakeOrder(*group, pending, 0.0, 0.0, false);
  const bool already_unknown =
      std::find(group->unknown_result_order_ids.begin(),
                group->unknown_result_order_ids.end(),
                pending.local_order_id) !=
      group->unknown_result_order_ids.end();
  const bool can_auto_recover =
      !model->needs_reconcile || model->unknown_result_reconcile_active;
  const bool expected = !already_unknown;
  if (expected) {
    group->unknown_result_order_ids.push_back(pending.local_order_id);
    ++model->unknown_result_count;
  }
  model->recovery_state = leadlag::RecoveryState::kDegradedNeedsReconcile;
  model->unknown_result_reconcile_active = can_auto_recover;
  model->needs_reconcile = true;
  EXPECT_EQ(actual->MarkUnknownResult(order), expected);
}

void ApplyContinuityLost(leadlag::ExecutionState* actual,
                         ReferenceModel* model) {
  actual->OnFeedbackContinuityLost(aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kContinuityLost,
  });
  if (model->recovery_state != leadlag::RecoveryState::kManualIntervention) {
    model->recovery_state = leadlag::RecoveryState::kDegradedNeedsReconcile;
  }
  model->needs_reconcile = true;
  model->unknown_result_reconcile_active = false;
}

void AdvanceRecovery(leadlag::ExecutionState* actual, ReferenceModel* model) {
  if (model->recovery_state ==
      leadlag::RecoveryState::kDegradedNeedsReconcile) {
    EXPECT_TRUE(actual->BeginReconcile());
    model->recovery_state = leadlag::RecoveryState::kReconciling;
    model->needs_reconcile = true;
    model->unknown_result_reconcile_active = false;
    return;
  }
  if (model->recovery_state != leadlag::RecoveryState::kReconciling) {
    return;
  }
  const leadlag::RecoveryApplyResult success{
      .recovered = true,
      .position_match = true,
      .open_orders_resolved = true,
      .terminal_facts_resolved = true,
      .manual_intervention = false,
  };
  EXPECT_TRUE(actual->ApplyRecoveryResult(success));
  model->recovery_state = leadlag::RecoveryState::kNormal;
  model->needs_reconcile = false;
  for (const leadlag::ExecutionGroupIndex index : model->active_indices) {
    model->groups[index].unknown_result_order_ids.clear();
  }
  model->unknown_result_count = 0;
  model->unknown_result_reconcile_active = false;
  model->last_unknown_result_auto_recovered = false;
}

void ClearRandomGroup(leadlag::ExecutionState* actual, ReferenceModel* model,
                      std::mt19937_64* random) {
  ModelGroup* group = RandomActiveGroup(model, random);
  if (group == nullptr) {
    return;
  }
  const std::uint64_t group_id = group->group_id;
  EXPECT_TRUE(actual->ClearGroupById(group_id));
  RemoveModelGroup(model, group);
}

void ReplayRandomRetiredOrder(leadlag::ExecutionState* actual,
                              ReferenceModel* model,
                              const std::vector<RetiredOrder>& retired,
                              std::mt19937_64* random) {
  if (retired.empty()) {
    return;
  }
  const aquila::core::StrategyOrder& order =
      retired[(*random)() % retired.size()].order;
  const leadlag::ExecutionApplyResult expected =
      ApplyModelTerminal(model, order);
  EXPECT_EQ(actual->ApplyTerminalOrder(order, Instrument()), expected);
}

void ApplyActiveMismatch(leadlag::ExecutionState* actual, ReferenceModel* model,
                         std::mt19937_64* random) {
  if (model->active_indices.size() < 2U) {
    return;
  }
  ModelGroup* source = RandomGroupWithPending(model, random);
  if (source == nullptr) {
    return;
  }
  ModelGroup* other = nullptr;
  for (const leadlag::ExecutionGroupIndex index : model->active_indices) {
    if (model->groups[index].group_id != source->group_id) {
      other = &model->groups[index];
      break;
    }
  }
  ASSERT_NE(other, nullptr);
  const ModelPendingOrder& pending =
      source->pending[(*random)() % source->pending.size()];
  aquila::core::StrategyOrder order = MakeOrder(*source, pending, 1.0, 100.0);
  order.place_request.group_id = other->group_id;
  EXPECT_EQ(ApplyModelTerminal(model, order),
            leadlag::ExecutionApplyResult::kIgnoredGroupMismatch);
  EXPECT_EQ(actual->ApplyTerminalOrder(order, Instrument()),
            leadlag::ExecutionApplyResult::kIgnoredGroupMismatch);
}

void ApplyRandomNonTerminal(leadlag::ExecutionState* actual,
                            ReferenceModel* model, std::mt19937_64* random) {
  ModelGroup* group = RandomGroupWithPending(model, random);
  if (group == nullptr) {
    return;
  }
  const ModelPendingOrder& pending =
      group->pending[(*random)() % group->pending.size()];
  const aquila::core::StrategyOrder order =
      MakeOrder(*group, pending, 1.0, 100.0, false);
  EXPECT_EQ(ApplyModelTerminal(model, order),
            leadlag::ExecutionApplyResult::kIgnoredNonTerminal);
  EXPECT_EQ(actual->ApplyTerminalOrder(order, Instrument()),
            leadlag::ExecutionApplyResult::kIgnoredNonTerminal);
}

void RunSeed(std::size_t capacity, std::uint64_t seed) {
  leadlag::ExecutionState actual;
  ASSERT_TRUE(actual.Init(static_cast<std::uint32_t>(capacity)));
  ReferenceModel model{.capacity = capacity};
  std::mt19937_64 random(seed);
  std::uint64_t next_order_id = 1;
  std::vector<RetiredOrder> retired;
  retired.reserve(10'000);

  for (std::size_t step = 0; step < 10'000; ++step) {
    const std::uint64_t event = random() % 13U;
    SCOPED_TRACE(::testing::Message()
                 << "capacity=" << capacity << " seed=" << seed
                 << " step=" << step << " event=" << event);
    switch (event) {
      case 0:
        CreateOpen(&actual, &model, &next_order_id, &random);
        break;
      case 1:
        CreateHold(&actual, &model, &random);
        break;
      case 2:
        AddOpenChild(&actual, &model, &next_order_id, &random);
        break;
      case 3:
        StartClose(&actual, &model, &next_order_id, &random);
        break;
      case 4:
      case 5:
        ApplyRandomTerminal(&actual, &model, &retired, &random);
        break;
      case 6:
        ApplyRandomRejected(&actual, &model, &retired, &random);
        break;
      case 7:
        MarkRandomUnknown(&actual, &model, &random);
        break;
      case 8:
        ApplyContinuityLost(&actual, &model);
        break;
      case 9:
        AdvanceRecovery(&actual, &model);
        break;
      case 10:
        ClearRandomGroup(&actual, &model, &random);
        break;
      case 11:
        ReplayRandomRetiredOrder(&actual, &model, retired, &random);
        break;
      case 12:
        if ((random() & 1U) == 0U) {
          ApplyActiveMismatch(&actual, &model, &random);
        } else {
          ApplyRandomNonTerminal(&actual, &model, &random);
        }
        break;
    }
    CompareState(actual, model);
  }

  if (model.recovery_state == leadlag::RecoveryState::kNormal) {
    while (!model.active_indices.empty()) {
      ModelGroup* group = &model.groups[model.active_indices.front()];
      if (!group->pending.empty()) {
        const ModelPendingOrder pending = group->pending.front();
        aquila::core::StrategyOrder order =
            MakeOrder(*group, pending, 0.0, 0.0);
        EXPECT_EQ(actual.ApplyTerminalOrder(order, Instrument()),
                  ApplyModelTerminal(&model, order));
      } else if (std::abs(group->signed_position_quantity) >
                 leadlag::kExecutionQuantityEpsilon) {
        const std::uint64_t local_order_id = next_order_id++;
        leadlag::ExecutionGroup* actual_group =
            actual.GroupAt(group->group_index, group->group_id);
        ASSERT_NE(actual_group, nullptr);
        ASSERT_TRUE(actual.StartCloseOrder(*actual_group, local_order_id,
                                           leadlag::CloseOrderKind::kStoploss));
        group->stage = leadlag::ExecutionStage::kClose;
        group->close_order_kind = leadlag::CloseOrderKind::kStoploss;
        group->close_base_position_quantity =
            std::abs(group->signed_position_quantity);
        const ModelPendingOrder pending{
            .local_order_id = local_order_id,
            .role = leadlag::PendingOrderRole::kStoplossClose,
            .side = group->signed_position_quantity > 0.0
                        ? aquila::OrderSide::kSell
                        : aquila::OrderSide::kBuy,
        };
        AddPendingOrder(group, pending.local_order_id, pending.role,
                        pending.side);
        const aquila::core::StrategyOrder order = MakeOrder(
            *group, pending, std::abs(group->signed_position_quantity), 100.0);
        EXPECT_EQ(actual.ApplyTerminalOrder(order, Instrument()),
                  ApplyModelTerminal(&model, order));
      } else {
        const std::uint64_t group_id = group->group_id;
        EXPECT_TRUE(actual.ClearGroupById(group_id));
        RemoveModelGroup(&model, group);
      }
      CompareState(actual, model);
    }
    EXPECT_EQ(actual.active_group_count(), 0U);
  }
}

TEST(LeadLagExecutionStateModelTest,
     FixedSeedsMatchReferenceAtParallelTwoFourEightAndSixteen) {
  constexpr std::array<std::size_t, 4> kCapacities{2, 4, 8, 16};
  constexpr std::array<std::uint64_t, 16> kSeeds{
      0x11U,    0x23U,    0x47U,    0x89U,    0x101U,  0x203U,
      0x407U,   0x809U,   0x1009U,  0x2011U,  0x4021U, 0x8041U,
      0x10081U, 0x20101U, 0x40201U, 0x80401U,
  };
  for (const std::size_t capacity : kCapacities) {
    for (const std::uint64_t seed : kSeeds) {
      RunSeed(capacity, seed);
    }
  }
}

}  // namespace
