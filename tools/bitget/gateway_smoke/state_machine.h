#ifndef AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_STATE_MACHINE_H_
#define AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_STATE_MACHINE_H_

#include <cstdint>

#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "tools/bitget/gateway_smoke/types.h"

namespace aquila::tools::bitget::gateway_smoke {

class SmokeStateMachine {
 public:
  void MarkEntrySubmitted(std::uint64_t local_order_id,
                          std::int64_t submit_ns) noexcept;
  void MarkCloseSubmitted(std::uint64_t local_order_id, std::int64_t submit_ns,
                          double quantity) noexcept;
  void OnGatewayResponse(const core::OrderResponseEvent& event) noexcept;
  void OnFeedback(const OrderFeedbackEvent& event) noexcept;
  void CheckTimeout(std::int64_t now_ns, std::int64_t ack_timeout_ns,
                    std::int64_t terminal_timeout_ns) noexcept;

  [[nodiscard]] bool close_required() const noexcept {
    return close_required_ && !close_.submitted && !failed();
  }
  [[nodiscard]] bool done() const noexcept {
    return result_ != SmokeResult::kPending;
  }
  [[nodiscard]] bool failed() const noexcept {
    return failure_ != SmokeFailure::kNone;
  }
  [[nodiscard]] double entry_filled_quantity() const noexcept {
    return entry_.cumulative_filled_quantity;
  }
  [[nodiscard]] SmokeResult result() const noexcept {
    return result_;
  }
  [[nodiscard]] SmokeFailure failure() const noexcept {
    return failure_;
  }
  [[nodiscard]] const LegEvidence& entry() const noexcept {
    return entry_;
  }
  [[nodiscard]] const LegEvidence& close() const noexcept {
    return close_;
  }

 private:
  void ApplyGatewayResponse(LegEvidence* leg,
                            const core::OrderResponseEvent& event) noexcept;
  void ApplyFeedback(LegEvidence* leg,
                     const OrderFeedbackEvent& event) noexcept;
  void Evaluate() noexcept;
  void Fail(SmokeFailure failure) noexcept;

  LegEvidence entry_;
  LegEvidence close_;
  SmokeResult result_{SmokeResult::kPending};
  SmokeFailure failure_{SmokeFailure::kNone};
  bool close_required_{false};
};

}  // namespace aquila::tools::bitget::gateway_smoke

#endif  // AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_STATE_MACHINE_H_
