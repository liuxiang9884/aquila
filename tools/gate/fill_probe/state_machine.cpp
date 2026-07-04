#include "tools/gate/fill_probe/state_machine.h"

#include <cmath>

namespace aquila::tools::gate::fill_probe {
namespace {

inline constexpr std::int64_t kGtcCancelAfterNs = 1'000'000'000;
inline constexpr double kFlatEpsilon = 1e-9;

[[nodiscard]] bool IsFlat(double quantity) noexcept {
  return std::fabs(quantity) <= kFlatEpsilon;
}

[[nodiscard]] bool IsEntryTerminal(EntryResult result) noexcept {
  return result != EntryResult::kPending;
}

[[nodiscard]] bool IsCloseTerminal(CloseResult result) noexcept {
  return result != CloseResult::kPending;
}

}  // namespace

ProbeNode ProbeNode::Start(std::uint64_t node_id, NodeSide side,
                           std::int64_t decision_ns) {
  ProbeNode node;
  node.node_id_ = node_id;
  node.side_ = side;
  node.decision_ns_ = decision_ns;
  return node;
}

void ProbeNode::MarkEntrySubmitted(EntryKind kind, std::uint64_t local_order_id,
                                   std::uint16_t route_id,
                                   std::int64_t event_ns) {
  LifecycleState& lifecycle = Lifecycle(kind);
  if (lifecycle.entry_submitted) {
    return;
  }
  lifecycle.entry_submitted = true;
  lifecycle.entry_local_order_id = local_order_id;
  lifecycle.entry_route_id = route_id;
  lifecycle.entry_submit_ns = event_ns;
  lifecycle.entry_result = EntryResult::kPending;
}

void ProbeNode::MarkGtcCancelSubmitted(std::int64_t event_ns) {
  gtc_.gtc_cancel_submitted = true;
  gtc_.gtc_cancel_submit_ns = event_ns;
}

void ProbeNode::OnEntryTerminal(std::uint64_t local_order_id,
                                EntryResult result, double filled_qty,
                                double fill_price, std::int64_t event_ns) {
  LifecycleState* lifecycle = FindEntry(local_order_id);
  if (lifecycle == nullptr || lifecycle->entry_terminal) {
    return;
  }
  lifecycle->entry_result = result;
  lifecycle->entry_terminal = IsEntryTerminal(result);
  lifecycle->entry_finish_ns = event_ns;
  lifecycle->entry_filled_qty = filled_qty;
  lifecycle->entry_avg_fill_price = fill_price;

  if (filled_qty > 0.0) {
    total_entry_filled_qty_ += filled_qty;
    net_position_ += EntrySign() * filled_qty;
  }
  EvaluateCompletion(event_ns);
}

bool ProbeNode::GtcCancelDue(std::int64_t now_ns) const {
  return gtc_.entry_submitted && !gtc_.entry_terminal &&
         !gtc_.gtc_cancel_submitted &&
         now_ns - gtc_.entry_submit_ns > kGtcCancelAfterNs;
}

bool ProbeNode::CloseRetryAllowed(EntryKind kind,
                                  std::uint32_t max_retries) const {
  const LifecycleState& lifecycle = Lifecycle(kind);
  const double remaining_qty =
      lifecycle.entry_filled_qty - lifecycle.close_filled_qty;
  return status_ == NodeStatus::kRunning && remaining_qty > kFlatEpsilon &&
         !lifecycle.close_pending && lifecycle.close_attempts < max_retries;
}

void ProbeNode::MarkCloseSubmitted(EntryKind kind, std::uint64_t local_order_id,
                                   std::uint16_t route_id,
                                   std::int64_t event_ns) {
  LifecycleState& lifecycle = Lifecycle(kind);
  lifecycle.close_local_order_id = local_order_id;
  lifecycle.close_route_id = route_id;
  lifecycle.close_submit_ns = event_ns;
  lifecycle.close_pending = true;
  lifecycle.close_result = CloseResult::kPending;
  ++lifecycle.close_attempts;
}

void ProbeNode::OnCloseFill(std::uint64_t local_order_id, double filled_qty,
                            double fill_price, std::int64_t event_ns) {
  LifecycleState* lifecycle = FindClose(local_order_id);
  ApplyCloseFill(lifecycle, filled_qty, fill_price, event_ns);
}

void ProbeNode::OnCloseFill(EntryKind kind, double filled_qty,
                            double fill_price, std::int64_t event_ns) {
  ApplyCloseFill(&Lifecycle(kind), filled_qty, fill_price, event_ns);
}

void ProbeNode::OnCloseTerminal(std::uint64_t local_order_id,
                                CloseResult result, std::int64_t event_ns) {
  LifecycleState* lifecycle = FindClose(local_order_id);
  ApplyCloseTerminal(lifecycle, result, event_ns);
}

void ProbeNode::OnCloseTerminal(EntryKind kind, CloseResult result,
                                std::int64_t event_ns) {
  ApplyCloseTerminal(&Lifecycle(kind), result, event_ns);
}

bool ProbeNode::UnresolvedDue(std::int64_t now_ns,
                              std::int64_t timeout_ns) const {
  return status_ == NodeStatus::kRunning && timeout_ns > 0 &&
         now_ns - decision_ns_ > timeout_ns;
}

void ProbeNode::MarkUnresolved(std::int64_t event_ns) {
  if (status_ != NodeStatus::kRunning) {
    return;
  }
  status_ = NodeStatus::kUnresolved;
  finish_ns_ = event_ns;
}

LifecycleState& ProbeNode::Lifecycle(EntryKind kind) noexcept {
  return kind == EntryKind::kGtc ? gtc_ : ioc_;
}

const LifecycleState& ProbeNode::Lifecycle(EntryKind kind) const noexcept {
  return kind == EntryKind::kGtc ? gtc_ : ioc_;
}

LifecycleState* ProbeNode::FindEntry(std::uint64_t local_order_id) {
  if (gtc_.entry_local_order_id == local_order_id) {
    return &gtc_;
  }
  if (ioc_.entry_local_order_id == local_order_id) {
    return &ioc_;
  }
  return nullptr;
}

LifecycleState* ProbeNode::FindClose(std::uint64_t local_order_id) {
  if (gtc_.close_local_order_id == local_order_id) {
    return &gtc_;
  }
  if (ioc_.close_local_order_id == local_order_id) {
    return &ioc_;
  }
  return nullptr;
}

double ProbeNode::EntrySign() const noexcept {
  return side_ == NodeSide::kBuy ? 1.0 : -1.0;
}

void ProbeNode::ApplyCloseFill(LifecycleState* lifecycle, double filled_qty,
                               double fill_price, std::int64_t event_ns) {
  if (status_ != NodeStatus::kRunning || lifecycle == nullptr) {
    return;
  }
  lifecycle->close_pending = false;
  lifecycle->close_result = CloseResult::kFilled;
  lifecycle->close_finish_ns = event_ns;
  lifecycle->close_filled_qty += filled_qty;
  lifecycle->close_avg_fill_price = fill_price;
  if (filled_qty > 0.0) {
    net_position_ -= EntrySign() * filled_qty;
  }
  EvaluateCompletion(event_ns);
}

void ProbeNode::ApplyCloseTerminal(LifecycleState* lifecycle,
                                   CloseResult result, std::int64_t event_ns) {
  if (status_ != NodeStatus::kRunning || lifecycle == nullptr) {
    return;
  }
  lifecycle->close_pending = false;
  lifecycle->close_result = result;
  lifecycle->close_finish_ns = event_ns;
  if (IsCloseTerminal(result)) {
    EvaluateCompletion(event_ns);
  }
}

void ProbeNode::EvaluateCompletion(std::int64_t event_ns) {
  if (status_ != NodeStatus::kRunning) {
    return;
  }
  const bool entries_terminal = gtc_.entry_terminal && ioc_.entry_terminal;
  if (!entries_terminal) {
    return;
  }
  if (IsFlat(total_entry_filled_qty_)) {
    status_ = NodeStatus::kCompletedNoFill;
    finish_ns_ = event_ns;
    return;
  }
  if (IsFlat(net_position_)) {
    status_ = NodeStatus::kCompletedClosed;
    finish_ns_ = event_ns;
  }
}

}  // namespace aquila::tools::gate::fill_probe
