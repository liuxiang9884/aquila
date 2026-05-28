#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SESSION_STATE_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SESSION_STATE_H_

#include "exchange/gate/trading/order_types.h"

namespace aquila::tools::gate_order_session_rtt_probe {

enum class ProbeStage {
  kIdle,
  kGtcPlace,
  kGtcCancel,
  kGtcClose,
  kIocPlace,
  kIocClose,
};

enum class SafetyCloseStatus {
  kNotSubmitted,
  kAcked,
  kRejectedFlatSafe,
  kRejected,
  kTimeout,
  kSendFailed,
};

struct SafetyCloseInput {
  ProbeStage stage{ProbeStage::kIdle};
  gate::OrderResponseKind response_kind{gate::OrderResponseKind::kAck};
  bool terminal_uncertain{false};
  bool feedback_fill{false};
};

[[nodiscard]] inline bool ShouldSubmitGtcSafetyClose(
    const SafetyCloseInput& input) noexcept {
  if (input.feedback_fill || input.terminal_uncertain) {
    return true;
  }
  return input.stage == ProbeStage::kGtcCancel &&
         input.response_kind == gate::OrderResponseKind::kCancelRejected;
}

[[nodiscard]] inline bool ShouldSubmitIocSafetyCloseAfterAck(
    const SafetyCloseInput& input) noexcept {
  return input.stage == ProbeStage::kIocPlace &&
         input.response_kind == gate::OrderResponseKind::kAck;
}

[[nodiscard]] inline SafetyCloseStatus ClassifySafetyCloseRejected(
    bool position_known_flat) noexcept {
  return position_known_flat ? SafetyCloseStatus::kRejectedFlatSafe
                             : SafetyCloseStatus::kRejected;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SESSION_STATE_H_
