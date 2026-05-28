#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_FLOW_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_FLOW_H_

#include <cstdint>
#include <string>
#include <utility>

#include "core/common/types.h"
#include "exchange/gate/trading/order_types.h"
#include "tools/gate/order_session_rtt_probe/passive_order_builder.h"
#include "tools/gate/order_session_rtt_probe/session_state.h"

namespace aquila::tools::gate_order_session_rtt_probe {

struct ProbeSampleLocalIds {
  std::uint64_t gtc_local_order_id{0};
  std::uint64_t ioc_local_order_id{0};
  std::uint64_t gtc_close_local_order_id{0};
  std::uint64_t ioc_close_local_order_id{0};
};

struct ProbeWireOrder {
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::string symbol;
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  double quantity{0.0};
  std::string quantity_text;
  std::string price_text;
  bool reduce_only{false};
};

[[nodiscard]] inline ProbeWireOrder BuildGtcPlaceOrder(
    const PassiveOrderBuildResult& passive, const ProbeSampleLocalIds& ids) {
  return ProbeWireOrder{.local_order_id = ids.gtc_local_order_id,
                        .symbol = passive.contract,
                        .side = OrderSide::kBuy,
                        .type = OrderType::kLimit,
                        .time_in_force = TimeInForce::kGoodTillCancel,
                        .quantity_text = passive.quantity_text,
                        .price_text = passive.price_text,
                        .reduce_only = false};
}

[[nodiscard]] inline ProbeWireOrder BuildGtcCancelOrder(
    const ProbeWireOrder& gtc_place) {
  ProbeWireOrder cancel = gtc_place;
  cancel.time_in_force = TimeInForce::kGoodTillCancel;
  return cancel;
}

[[nodiscard]] inline ProbeWireOrder BuildIocPlaceOrder(
    const PassiveOrderBuildResult& passive, const ProbeSampleLocalIds& ids) {
  return ProbeWireOrder{.local_order_id = ids.ioc_local_order_id,
                        .symbol = passive.contract,
                        .side = OrderSide::kBuy,
                        .type = OrderType::kLimit,
                        .time_in_force = TimeInForce::kImmediateOrCancel,
                        .quantity_text = passive.quantity_text,
                        .price_text = passive.price_text,
                        .reduce_only = false};
}

[[nodiscard]] inline ProbeWireOrder BuildGtcCloseOrder(
    const PassiveOrderBuildResult& passive, const ProbeSampleLocalIds& ids) {
  return ProbeWireOrder{.local_order_id = ids.gtc_close_local_order_id,
                        .symbol = passive.contract,
                        .side = OrderSide::kSell,
                        .type = OrderType::kLimit,
                        .time_in_force = TimeInForce::kImmediateOrCancel,
                        .quantity_text = passive.quantity_text,
                        .price_text = "0",
                        .reduce_only = true};
}

[[nodiscard]] inline ProbeWireOrder BuildIocCloseOrder(
    const PassiveOrderBuildResult& passive, const ProbeSampleLocalIds& ids) {
  return ProbeWireOrder{.local_order_id = ids.ioc_close_local_order_id,
                        .symbol = passive.contract,
                        .side = OrderSide::kSell,
                        .type = OrderType::kLimit,
                        .time_in_force = TimeInForce::kImmediateOrCancel,
                        .quantity_text = passive.quantity_text,
                        .price_text = "0",
                        .reduce_only = true};
}

enum class ProbeSampleAction {
  kNone,
  kSubmitGtcPlace,
  kSubmitGtcCancel,
  kSubmitIocPlace,
  kSubmitGtcClose,
  kSubmitIocClose,
  kFinish,
  kFail,
};

struct ProbeSampleTransition {
  bool ok{true};
  ProbeSampleAction action{ProbeSampleAction::kNone};
  std::string error;
};

struct ProbeSampleStats {
  std::int64_t gtc_place_ack_rtt_ns{-1};
  std::int64_t gtc_cancel_ack_rtt_ns{-1};
  std::int64_t ioc_place_ack_rtt_ns{-1};
  std::int64_t gtc_close_ack_rtt_ns{-1};
  std::int64_t ioc_close_ack_rtt_ns{-1};
};

class ProbeSampleFlow {
 public:
  explicit ProbeSampleFlow(ProbeSampleLocalIds ids) : ids_(ids) {}

  [[nodiscard]] ProbeSampleAction Start() noexcept {
    started_ = true;
    return ProbeSampleAction::kSubmitGtcPlace;
  }

  [[nodiscard]] ProbeSampleTransition OnOrderSent(
      ProbeStage stage, const gate::OrderSendResult& sent) {
    if (sent.status != gate::OrderSendStatus::kOk) {
      return Fail("order send failed");
    }
    ProbeStageSendState* state = MutableSendState(stage);
    if (state == nullptr) {
      return Fail("unsupported probe stage");
    }
    *state = ProbeStageSendState{.sent = true,
                                 .request_sequence = sent.request_sequence,
                                 .send_local_ns = sent.send_local_ns};
    return ProbeSampleTransition{.ok = true};
  }

  [[nodiscard]] ProbeSampleTransition OnOrderResponse(
      const gate::OrderResponse& response) {
    if (response.kind != gate::OrderResponseKind::kAck) {
      return ProbeSampleTransition{.ok = true};
    }
    const ProbeStage stage = StageForRequestSequence(response.request_sequence);
    if (stage == ProbeStage::kIdle) {
      return ProbeSampleTransition{.ok = true};
    }
    const ProbeStageSendState* state = SendState(stage);
    if (state == nullptr || !state->sent) {
      return Fail("ack received before order send");
    }
    const std::int64_t rtt_ns =
        response.local_receive_ns - state->send_local_ns;
    if (rtt_ns < 0) {
      return Fail("negative ack rtt");
    }
    return RecordAckRtt(stage, rtt_ns);
  }

  [[nodiscard]] const ProbeSampleStats& stats() const noexcept {
    return stats_;
  }

  [[nodiscard]] const ProbeSampleLocalIds& ids() const noexcept {
    return ids_;
  }

  [[nodiscard]] bool started() const noexcept {
    return started_;
  }

 private:
  struct ProbeStageSendState {
    bool sent{false};
    std::uint64_t request_sequence{0};
    std::int64_t send_local_ns{0};
  };

  [[nodiscard]] static ProbeSampleTransition Fail(std::string error) {
    return ProbeSampleTransition{.ok = false,
                                 .action = ProbeSampleAction::kFail,
                                 .error = std::move(error)};
  }

  [[nodiscard]] ProbeStageSendState* MutableSendState(
      ProbeStage stage) noexcept {
    switch (stage) {
      case ProbeStage::kGtcPlace:
        return &gtc_place_;
      case ProbeStage::kGtcCancel:
        return &gtc_cancel_;
      case ProbeStage::kGtcClose:
        return &gtc_close_;
      case ProbeStage::kIocPlace:
        return &ioc_place_;
      case ProbeStage::kIocClose:
        return &ioc_close_;
      case ProbeStage::kIdle:
        return nullptr;
    }
    return nullptr;
  }

  [[nodiscard]] const ProbeStageSendState* SendState(
      ProbeStage stage) const noexcept {
    switch (stage) {
      case ProbeStage::kGtcPlace:
        return &gtc_place_;
      case ProbeStage::kGtcCancel:
        return &gtc_cancel_;
      case ProbeStage::kGtcClose:
        return &gtc_close_;
      case ProbeStage::kIocPlace:
        return &ioc_place_;
      case ProbeStage::kIocClose:
        return &ioc_close_;
      case ProbeStage::kIdle:
        return nullptr;
    }
    return nullptr;
  }

  [[nodiscard]] ProbeStage StageForRequestSequence(
      std::uint64_t request_sequence) const noexcept {
    if (gtc_place_.sent && gtc_place_.request_sequence == request_sequence) {
      return ProbeStage::kGtcPlace;
    }
    if (gtc_cancel_.sent && gtc_cancel_.request_sequence == request_sequence) {
      return ProbeStage::kGtcCancel;
    }
    if (gtc_close_.sent && gtc_close_.request_sequence == request_sequence) {
      return ProbeStage::kGtcClose;
    }
    if (ioc_place_.sent && ioc_place_.request_sequence == request_sequence) {
      return ProbeStage::kIocPlace;
    }
    if (ioc_close_.sent && ioc_close_.request_sequence == request_sequence) {
      return ProbeStage::kIocClose;
    }
    return ProbeStage::kIdle;
  }

  [[nodiscard]] ProbeSampleTransition RecordAckRtt(ProbeStage stage,
                                                   std::int64_t rtt_ns) {
    switch (stage) {
      case ProbeStage::kGtcPlace:
        stats_.gtc_place_ack_rtt_ns = rtt_ns;
        return ProbeSampleTransition{
            .ok = true, .action = ProbeSampleAction::kSubmitGtcCancel};
      case ProbeStage::kGtcCancel:
        stats_.gtc_cancel_ack_rtt_ns = rtt_ns;
        return ProbeSampleTransition{
            .ok = true, .action = ProbeSampleAction::kSubmitIocPlace};
      case ProbeStage::kIocPlace:
        stats_.ioc_place_ack_rtt_ns = rtt_ns;
        return ProbeSampleTransition{
            .ok = true, .action = ProbeSampleAction::kSubmitIocClose};
      case ProbeStage::kGtcClose:
        stats_.gtc_close_ack_rtt_ns = rtt_ns;
        return ProbeSampleTransition{.ok = true,
                                     .action = ProbeSampleAction::kFinish};
      case ProbeStage::kIocClose:
        stats_.ioc_close_ack_rtt_ns = rtt_ns;
        return ProbeSampleTransition{.ok = true,
                                     .action = ProbeSampleAction::kFinish};
      case ProbeStage::kIdle:
        break;
    }
    return ProbeSampleTransition{.ok = true};
  }

  ProbeSampleLocalIds ids_;
  bool started_{false};
  ProbeSampleStats stats_;
  ProbeStageSendState gtc_place_;
  ProbeStageSendState gtc_cancel_;
  ProbeStageSendState gtc_close_;
  ProbeStageSendState ioc_place_;
  ProbeStageSendState ioc_close_;
};

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_FLOW_H_
