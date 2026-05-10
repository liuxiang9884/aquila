#ifndef AQUILA_STRATEGY_LEAD_LAG_EXECUTION_STATE_H_
#define AQUILA_STRATEGY_LEAD_LAG_EXECUTION_STATE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aquila::strategy::leadlag {

enum class ExecutionStage : std::uint8_t {
  kIdle,
  kOpen,
  kHold,
  kClose,
};

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
    next_group_id_ = 1;
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
    return group;
  }

  [[nodiscard]] std::size_t active_group_count() const noexcept {
    std::size_t count = 0;
    for (const ExecutionGroup& group : groups_) {
      if (group.active()) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return groups_.size();
  }

  [[nodiscard]] std::vector<ExecutionGroup>& groups() noexcept {
    return groups_;
  }

  [[nodiscard]] const std::vector<ExecutionGroup>& groups() const noexcept {
    return groups_;
  }

 private:
  [[nodiscard]] ExecutionGroup* FindIdleGroup() noexcept {
    for (ExecutionGroup& group : groups_) {
      if (!group.active()) {
        return &group;
      }
    }
    return nullptr;
  }

  std::vector<ExecutionGroup> groups_;
  std::uint64_t next_group_id_{1};
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_EXECUTION_STATE_H_
