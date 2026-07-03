#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_STATE_MACHINE_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_STATE_MACHINE_H_

#include <cstdint>

namespace aquila::tools::gate::fill_probe {

enum class NodeSide : std::uint8_t { kBuy, kSell };
enum class EntryKind : std::uint8_t { kGtc, kIoc };
enum class EntryResult : std::uint8_t {
  kPending,
  kFilled,
  kPartialFilled,
  kCancelled,
  kRejected,
  kUnknown,
};
enum class CloseResult : std::uint8_t {
  kPending,
  kFilled,
  kPartialFilled,
  kCancelled,
  kRejected,
  kUnknown,
};
enum class NodeStatus : std::uint8_t {
  kRunning,
  kCompletedNoFill,
  kCompletedClosed,
  kUnresolved,
};

struct LifecycleState {
  EntryKind kind{EntryKind::kGtc};
  std::uint64_t entry_local_order_id{0};
  std::uint64_t close_local_order_id{0};
  std::uint16_t entry_route_id{0};
  std::uint16_t close_route_id{0};
  std::int64_t entry_submit_ns{0};
  std::int64_t entry_finish_ns{0};
  std::int64_t gtc_cancel_submit_ns{0};
  std::int64_t close_submit_ns{0};
  std::int64_t close_finish_ns{0};
  EntryResult entry_result{EntryResult::kPending};
  CloseResult close_result{CloseResult::kPending};
  double entry_filled_qty{0.0};
  double entry_avg_fill_price{0.0};
  double close_filled_qty{0.0};
  double close_avg_fill_price{0.0};
  std::uint32_t close_attempts{0};
  bool entry_submitted{false};
  bool entry_terminal{false};
  bool close_pending{false};
  bool gtc_cancel_submitted{false};
};

class ProbeNode {
 public:
  [[nodiscard]] static ProbeNode Start(std::uint64_t node_id, NodeSide side,
                                       std::int64_t decision_ns);

  void MarkEntrySubmitted(EntryKind kind, std::uint64_t local_order_id,
                          std::uint16_t route_id, std::int64_t event_ns);
  void MarkGtcCancelSubmitted(std::int64_t event_ns);
  void OnEntryTerminal(std::uint64_t local_order_id, EntryResult result,
                       double filled_qty, double fill_price,
                       std::int64_t event_ns);

  [[nodiscard]] bool GtcCancelDue(std::int64_t now_ns) const;

  [[nodiscard]] bool CloseRetryAllowed(EntryKind kind,
                                       std::uint32_t max_retries) const;
  void MarkCloseSubmitted(EntryKind kind, std::uint64_t local_order_id,
                          std::uint16_t route_id, std::int64_t event_ns);
  void OnCloseFill(std::uint64_t local_order_id, double filled_qty,
                   double fill_price, std::int64_t event_ns);
  void OnCloseTerminal(std::uint64_t local_order_id, CloseResult result,
                       std::int64_t event_ns);

  [[nodiscard]] bool UnresolvedDue(std::int64_t now_ns) const;
  void MarkUnresolved(std::int64_t event_ns);

  [[nodiscard]] bool Done() const noexcept {
    return status_ != NodeStatus::kRunning;
  }

  [[nodiscard]] NodeStatus status() const noexcept {
    return status_;
  }
  [[nodiscard]] double net_position() const noexcept {
    return net_position_;
  }
  [[nodiscard]] std::uint64_t node_id() const noexcept {
    return node_id_;
  }
  [[nodiscard]] NodeSide side() const noexcept {
    return side_;
  }
  [[nodiscard]] std::int64_t decision_ns() const noexcept {
    return decision_ns_;
  }
  [[nodiscard]] std::int64_t finish_ns() const noexcept {
    return finish_ns_;
  }
  [[nodiscard]] const LifecycleState& gtc() const noexcept {
    return gtc_;
  }
  [[nodiscard]] const LifecycleState& ioc() const noexcept {
    return ioc_;
  }

 private:
  [[nodiscard]] LifecycleState& Lifecycle(EntryKind kind) noexcept;
  [[nodiscard]] const LifecycleState& Lifecycle(EntryKind kind) const noexcept;
  [[nodiscard]] LifecycleState* FindEntry(std::uint64_t local_order_id);
  [[nodiscard]] LifecycleState* FindClose(std::uint64_t local_order_id);
  [[nodiscard]] double EntrySign() const noexcept;
  void EvaluateCompletion(std::int64_t event_ns);

  std::uint64_t node_id_{0};
  NodeSide side_{NodeSide::kBuy};
  std::int64_t decision_ns_{0};
  std::int64_t finish_ns_{0};
  NodeStatus status_{NodeStatus::kRunning};
  LifecycleState gtc_{.kind = EntryKind::kGtc};
  LifecycleState ioc_{.kind = EntryKind::kIoc};
  double net_position_{0.0};
  double total_entry_filled_qty_{0.0};
};

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_STATE_MACHINE_H_
