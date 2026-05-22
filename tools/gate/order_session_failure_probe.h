#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_FAILURE_PROBE_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_FAILURE_PROBE_H_

#include <string_view>

#include "exchange/gate/trading/order_types.h"

namespace aquila::tools::gate_order_session_failure_probe {

enum class ProbeMode {
  kSubmitRejected,
  kCancelRejected,
};

struct ProbeResponseInput {
  ProbeMode mode{ProbeMode::kCancelRejected};
  gate::OrderResponseKind kind{gate::OrderResponseKind::kAck};
  bool keep_open{false};
  bool safety_cancel_submitted{false};
};

struct ProbeResponseDecision {
  bool finish{false};
  bool submit_safety_cancel{false};
  int exit_code{1};
};

[[nodiscard]] inline bool ParseProbeMode(std::string_view value,
                                         ProbeMode* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  if (value == "submit-rejected") {
    *output = ProbeMode::kSubmitRejected;
    return true;
  }
  if (value == "cancel-rejected") {
    *output = ProbeMode::kCancelRejected;
    return true;
  }
  return false;
}

[[nodiscard]] inline std::string_view ProbeModeName(ProbeMode mode) noexcept {
  switch (mode) {
    case ProbeMode::kSubmitRejected:
      return "submit-rejected";
    case ProbeMode::kCancelRejected:
      return "cancel-rejected";
  }
  return "cancel-rejected";
}

[[nodiscard]] inline ProbeResponseDecision ResolveProbeResponseDecision(
    const ProbeResponseInput& input) noexcept {
  if (input.kind == gate::OrderResponseKind::kAck) {
    return {};
  }

  if (input.mode == ProbeMode::kCancelRejected) {
    return {.finish = true,
            .exit_code =
                input.kind == gate::OrderResponseKind::kCancelRejected ? 0 : 1};
  }

  if (input.kind == gate::OrderResponseKind::kRejected) {
    return {.finish = true, .exit_code = 0};
  }
  if (input.kind == gate::OrderResponseKind::kAccepted && !input.keep_open &&
      !input.safety_cancel_submitted) {
    return {.submit_safety_cancel = true};
  }
  return {.finish = true, .exit_code = 1};
}

}  // namespace aquila::tools::gate_order_session_failure_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_FAILURE_PROBE_H_
