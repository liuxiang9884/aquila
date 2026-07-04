#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_NODE_BUDGET_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_NODE_BUDGET_H_

#include <cstdint>

namespace aquila::tools::gate::fill_probe {

class SubmittedNodeBudget {
 public:
  explicit SubmittedNodeBudget(std::uint64_t max_submitted_nodes) noexcept
      : max_submitted_nodes_(max_submitted_nodes) {}

  [[nodiscard]] bool CanSubmitNode() const noexcept {
    return submitted_nodes_ < max_submitted_nodes_;
  }

  [[nodiscard]] std::uint64_t ReserveSubmittedNode() noexcept {
    if (CanSubmitNode()) {
      ++submitted_nodes_;
    }
    return submitted_nodes_;
  }

  [[nodiscard]] std::uint64_t submitted_nodes() const noexcept {
    return submitted_nodes_;
  }

 private:
  std::uint64_t max_submitted_nodes_{0};
  std::uint64_t submitted_nodes_{0};
};

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_NODE_BUDGET_H_
