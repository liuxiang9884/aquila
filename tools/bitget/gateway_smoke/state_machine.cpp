#include "tools/bitget/gateway_smoke/state_machine.h"

#include <algorithm>
#include <cmath>

namespace aquila::tools::bitget::gateway_smoke {
namespace {

inline constexpr double kQuantityEpsilon = 1e-12;

[[nodiscard]] bool IsTerminal(OrderFeedbackKind kind) noexcept {
  return kind == OrderFeedbackKind::kFilled ||
         kind == OrderFeedbackKind::kCancelled ||
         kind == OrderFeedbackKind::kRejected;
}

}  // namespace

void SmokeStateMachine::MarkEntrySubmitted(std::uint64_t local_order_id,
                                           std::int64_t submit_ns) noexcept {
  if (failed() || done() || entry_.submitted || local_order_id == 0) {
    Fail(SmokeFailure::kInvalidTransition);
    return;
  }
  entry_.local_order_id = local_order_id;
  entry_.submit_ns = submit_ns;
  entry_.submitted = true;
}

void SmokeStateMachine::MarkCloseSubmitted(std::uint64_t local_order_id,
                                           std::int64_t submit_ns,
                                           double quantity) noexcept {
  if (failed() || done() || !close_required_ || close_.submitted ||
      local_order_id == 0 || quantity <= 0.0 ||
      std::fabs(quantity - entry_.cumulative_filled_quantity) >
          kQuantityEpsilon) {
    Fail(SmokeFailure::kInvalidTransition);
    return;
  }
  close_.local_order_id = local_order_id;
  close_.submit_ns = submit_ns;
  close_.requested_quantity = quantity;
  close_.submitted = true;
}

void SmokeStateMachine::OnGatewayResponse(
    const core::OrderResponseEvent& event) noexcept {
  if (failed() || done()) {
    return;
  }
  if (entry_.submitted && event.local_order_id == entry_.local_order_id) {
    ApplyGatewayResponse(&entry_, event);
  } else if (close_.submitted &&
             event.local_order_id == close_.local_order_id) {
    ApplyGatewayResponse(&close_, event);
  }
  Evaluate();
}

void SmokeStateMachine::OnFeedback(const OrderFeedbackEvent& event) noexcept {
  if (failed() || done()) {
    return;
  }
  if (event.kind == OrderFeedbackKind::kContinuityLost) {
    Fail(SmokeFailure::kFeedbackContinuityLost);
    return;
  }
  if (entry_.submitted && event.local_order_id == entry_.local_order_id) {
    ApplyFeedback(&entry_, event);
  } else if (close_.submitted &&
             event.local_order_id == close_.local_order_id) {
    ApplyFeedback(&close_, event);
  }
  Evaluate();
}

void SmokeStateMachine::CheckTimeout(
    std::int64_t now_ns, std::int64_t ack_timeout_ns,
    std::int64_t terminal_timeout_ns) noexcept {
  if (failed() || done()) {
    return;
  }
  LegEvidence* leg = close_.submitted ? &close_ : &entry_;
  if (!leg->submitted || now_ns < leg->submit_ns) {
    return;
  }
  if (!leg->acked && now_ns - leg->submit_ns > ack_timeout_ns) {
    Fail(SmokeFailure::kAckTimeout);
    return;
  }
  const std::int64_t terminal_start_ns =
      leg->acked ? leg->ack_ns : leg->submit_ns;
  if (!leg->terminal && leg->acked && now_ns >= terminal_start_ns &&
      now_ns - terminal_start_ns > terminal_timeout_ns) {
    Fail(SmokeFailure::kTerminalTimeout);
  }
}

void SmokeStateMachine::ApplyGatewayResponse(
    LegEvidence* leg, const core::OrderResponseEvent& event) noexcept {
  switch (event.kind) {
    case core::OrderResponseKind::kAck:
      leg->acked = true;
      leg->ack_ns = event.local_receive_ns;
      return;
    case core::OrderResponseKind::kRejected:
      Fail(SmokeFailure::kGatewayRejected);
      return;
    case core::OrderResponseKind::kUnknownResult:
      Fail(SmokeFailure::kGatewayUnknown);
      return;
    case core::OrderResponseKind::kAccepted:
    case core::OrderResponseKind::kCancelAccepted:
    case core::OrderResponseKind::kCancelRejected:
      return;
  }
}

void SmokeStateMachine::ApplyFeedback(
    LegEvidence* leg, const OrderFeedbackEvent& event) noexcept {
  if (leg->terminal) {
    return;
  }
  leg->cumulative_filled_quantity = std::max(leg->cumulative_filled_quantity,
                                             event.cumulative_filled_quantity);
  if (!IsTerminal(event.kind)) {
    return;
  }
  if (event.kind == OrderFeedbackKind::kRejected) {
    Fail(SmokeFailure::kFeedbackRejected);
    return;
  }
  leg->terminal = true;
  leg->terminal_ns = event.local_receive_ns;
}

void SmokeStateMachine::Evaluate() noexcept {
  if (failed() || done() || !entry_.submitted || !entry_.acked ||
      !entry_.terminal) {
    return;
  }
  if (entry_.cumulative_filled_quantity <= kQuantityEpsilon) {
    result_ = SmokeResult::kNoFill;
    return;
  }
  close_required_ = true;
  if (!close_.submitted || !close_.acked || !close_.terminal) {
    return;
  }
  if (close_.cumulative_filled_quantity + kQuantityEpsilon <
      entry_.cumulative_filled_quantity) {
    Fail(SmokeFailure::kCloseResidual);
    return;
  }
  result_ = SmokeResult::kClosed;
}

void SmokeStateMachine::Fail(SmokeFailure failure) noexcept {
  if (failure_ == SmokeFailure::kNone && result_ == SmokeResult::kPending) {
    failure_ = failure;
  }
}

}  // namespace aquila::tools::bitget::gateway_smoke
