#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SAMPLE_FLOW_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SAMPLE_FLOW_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "core/trading/order_feedback_event.h"
#include "exchange/bitget/trading/order_types.h"
#include "tools/bitget/order_session_rtt_probe/sample_id_allocator.h"

namespace aquila::tools::bitget_order_session_rtt_probe {

enum class ProbeSampleAction : std::uint8_t {
  kNone,
  kSubmitIoc,
  kSubmitSafetyClose,
  kComplete,
  kFail,
};

struct ProbeSampleTransition {
  bool ok{true};
  ProbeSampleAction action{ProbeSampleAction::kNone};
  std::string error;
};

struct ProbeSampleStats {
  std::uint64_t ioc_local_order_id{0};
  std::uint64_t close_local_order_id{0};
  std::uint64_t request_sequence{0};
  std::uint64_t close_request_sequence{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t connection_id_hash{0};
  std::uint32_t error_code{0};
  std::int64_t request_send_ns{0};
  std::int64_t response_receive_ns{0};
  std::int64_t response_exchange_ns{0};
  std::int64_t ack_rtt_ns{-1};
  std::int64_t terminal_feedback_local_ns{0};
  std::int64_t terminal_feedback_exchange_ns{0};
  OrderFeedbackKind terminal_feedback_kind{OrderFeedbackKind::kCancelled};
  OrderFinishReason terminal_finish_reason{OrderFinishReason::kUnknown};
  bool terminal_feedback_observed{false};
  bool place_ack_observed{false};
  bool zero_fill_cancelled_observed{false};
  bool normal_terminal_confirmed{false};
  bool unexpected_fill{false};
  bool safety_close_requested{false};
  bool safety_close_sent{false};
  bool safety_close_confirmed{false};
  bool completed{false};
  bool invalid{false};
  double observed_fill_quantity{0.0};
  double safety_close_filled_quantity{0.0};
  std::string invalid_reason;
};

class ProbeSampleFlow {
 public:
  explicit ProbeSampleFlow(ProbeSampleLocalIds ids) noexcept : ids_(ids) {
    stats_.ioc_local_order_id = ids.ioc_local_order_id;
    stats_.close_local_order_id = ids.close_local_order_id;
  }

  [[nodiscard]] ProbeSampleTransition Start() noexcept {
    if (started_) {
      return Fail("sample already started");
    }
    started_ = true;
    return {.action = ProbeSampleAction::kSubmitIoc};
  }

  [[nodiscard]] ProbeSampleTransition OnOrderSent(
      const bitget::OrderSendResult& sent) {
    if (!started_ || place_sent_) {
      return Fail("IOC send is out of sequence");
    }
    if (sent.status != bitget::OrderSendStatus::kOk) {
      return Fail("IOC order send failed");
    }
    place_sent_ = true;
    stats_.request_sequence = sent.request_sequence;
    stats_.request_send_ns = sent.send_local_ns;
    return {};
  }

  [[nodiscard]] ProbeSampleTransition OnSafetyCloseSent(
      const bitget::OrderSendResult& sent) {
    if (!stats_.safety_close_requested || stats_.safety_close_sent) {
      return Fail("safety close send is out of sequence");
    }
    if (sent.status != bitget::OrderSendStatus::kOk) {
      return Fail("safety close send failed");
    }
    stats_.safety_close_sent = true;
    stats_.close_request_sequence = sent.request_sequence;
    return {};
  }

  [[nodiscard]] ProbeSampleTransition OnOrderResponse(
      const bitget::OrderResponse& response) {
    if (stats_.completed) {
      return {};
    }
    if (response.request_type != bitget::OrderRequestType::kPlaceOrder) {
      return Fail("order response request_type mismatch");
    }
    if (place_sent_ && response.request_sequence == stats_.request_sequence) {
      if (response.local_order_id != ids_.ioc_local_order_id) {
        return Fail("IOC response local_order_id mismatch");
      }
      return HandlePlaceResponse(response);
    }
    if (stats_.safety_close_sent &&
        response.request_sequence == stats_.close_request_sequence) {
      if (response.local_order_id != ids_.close_local_order_id) {
        return Fail("safety close response local_order_id mismatch");
      }
      if (response.kind != bitget::OrderResponseKind::kAck) {
        return Fail(ResponseFailure("safety close", response.kind));
      }
      return {};
    }
    return Fail("unmapped order response sequence");
  }

  [[nodiscard]] ProbeSampleTransition OnOrderFeedback(
      const OrderFeedbackEvent& feedback) {
    if (feedback.kind == OrderFeedbackKind::kContinuityLost) {
      return Fail("feedback continuity lost");
    }
    if (stats_.completed) {
      return {};
    }
    if (feedback.local_order_id == ids_.ioc_local_order_id) {
      return HandlePlaceFeedback(feedback);
    }
    if (feedback.local_order_id == ids_.close_local_order_id) {
      return HandleCloseFeedback(feedback);
    }
    return {};
  }

  [[nodiscard]] ProbeSampleTransition OnTimeout() {
    return Fail("sample timeout");
  }

  [[nodiscard]] const ProbeSampleStats& stats() const noexcept {
    return stats_;
  }

 private:
  [[nodiscard]] ProbeSampleTransition HandlePlaceResponse(
      const bitget::OrderResponse& response) {
    if (response.kind != bitget::OrderResponseKind::kAck) {
      return Fail(ResponseFailure("IOC", response.kind));
    }
    if (response.local_receive_ns < stats_.request_send_ns) {
      return Fail("IOC response has negative RTT");
    }
    stats_.place_ack_observed = true;
    stats_.exchange_order_id = response.exchange_order_id;
    stats_.connection_id_hash = response.connection_id_hash;
    stats_.error_code = response.error_code;
    stats_.response_receive_ns = response.local_receive_ns;
    stats_.response_exchange_ns = response.exchange_ns;
    stats_.ack_rtt_ns = response.local_receive_ns - stats_.request_send_ns;
    if (stats_.zero_fill_cancelled_observed) {
      return CompleteNormal();
    }
    return {};
  }

  [[nodiscard]] ProbeSampleTransition HandlePlaceFeedback(
      const OrderFeedbackEvent& feedback) {
    stats_.terminal_feedback_observed =
        stats_.terminal_feedback_observed || IsTerminal(feedback.kind);
    if (IsTerminal(feedback.kind)) {
      stats_.terminal_feedback_kind = feedback.kind;
      stats_.terminal_finish_reason = feedback.finish_reason;
      stats_.terminal_feedback_local_ns = feedback.local_receive_ns;
      stats_.terminal_feedback_exchange_ns = feedback.exchange_update_ns;
    }
    if (feedback.kind == OrderFeedbackKind::kPartialFilled ||
        feedback.kind == OrderFeedbackKind::kFilled ||
        feedback.cumulative_filled_quantity > 0.0) {
      stats_.unexpected_fill = true;
      stats_.invalid = true;
      SetInvalidReason("unexpected IOC fill");
      stats_.observed_fill_quantity = std::max(
          stats_.observed_fill_quantity, feedback.cumulative_filled_quantity);
      if (!stats_.safety_close_requested) {
        stats_.safety_close_requested = true;
        return {.action = ProbeSampleAction::kSubmitSafetyClose};
      }
      return {};
    }
    if (feedback.kind == OrderFeedbackKind::kCancelled) {
      stats_.zero_fill_cancelled_observed = true;
      if (stats_.place_ack_observed) {
        return CompleteNormal();
      }
      return {};
    }
    if (feedback.kind == OrderFeedbackKind::kRejected) {
      return Fail("IOC rejected by feedback");
    }
    return {};
  }

  [[nodiscard]] ProbeSampleTransition HandleCloseFeedback(
      const OrderFeedbackEvent& feedback) {
    if (!stats_.safety_close_sent) {
      return Fail("safety close feedback arrived before send");
    }
    stats_.safety_close_filled_quantity =
        std::max(stats_.safety_close_filled_quantity,
                 feedback.cumulative_filled_quantity);
    if (feedback.kind == OrderFeedbackKind::kFilled) {
      if (stats_.safety_close_filled_quantity + kQuantityEpsilon >=
          stats_.observed_fill_quantity) {
        stats_.safety_close_confirmed = true;
        stats_.completed = true;
        return {.action = ProbeSampleAction::kComplete};
      }
      return Fail("safety close terminal did not prove flat");
    }
    if (feedback.kind == OrderFeedbackKind::kCancelled ||
        feedback.kind == OrderFeedbackKind::kRejected) {
      return Fail("safety close terminal did not prove flat");
    }
    return {};
  }

  [[nodiscard]] ProbeSampleTransition CompleteNormal() noexcept {
    stats_.normal_terminal_confirmed = true;
    stats_.completed = true;
    return {.action = ProbeSampleAction::kComplete};
  }

  [[nodiscard]] ProbeSampleTransition Fail(std::string error) {
    stats_.invalid = true;
    SetInvalidReason(error);
    return {.ok = false,
            .action = ProbeSampleAction::kFail,
            .error = std::move(error)};
  }

  void SetInvalidReason(std::string_view reason) {
    if (stats_.invalid_reason.empty()) {
      stats_.invalid_reason.assign(reason);
    }
  }

  [[nodiscard]] static bool IsTerminal(OrderFeedbackKind kind) noexcept {
    return kind == OrderFeedbackKind::kFilled ||
           kind == OrderFeedbackKind::kCancelled ||
           kind == OrderFeedbackKind::kRejected;
  }

  [[nodiscard]] static std::string ResponseFailure(
      std::string_view stage, bitget::OrderResponseKind kind) {
    switch (kind) {
      case bitget::OrderResponseKind::kRejected:
        return std::string(stage) + " response rejected";
      case bitget::OrderResponseKind::kCancelRejected:
        return std::string(stage) + " response cancel rejected";
      case bitget::OrderResponseKind::kUnknownResult:
        return std::string(stage) + " response unknown result";
      case bitget::OrderResponseKind::kAck:
        break;
    }
    return std::string(stage) + " response failure";
  }

  inline static constexpr double kQuantityEpsilon = 1e-12;

  ProbeSampleLocalIds ids_;
  ProbeSampleStats stats_{};
  bool started_{false};
  bool place_sent_{false};
};

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SAMPLE_FLOW_H_
