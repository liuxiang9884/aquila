#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_EXECUTOR_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_EXECUTOR_H_

#include <memory>
#include <string>
#include <utility>

#include "core/common/result.h"
#include "exchange/gate/trading/order_types.h"
#include "tools/gate/order_session_rtt_probe/passive_order_builder.h"
#include "tools/gate/order_session_rtt_probe/sample_flow.h"
#include "tools/gate/order_session_rtt_probe/session_state.h"

namespace aquila::tools::gate_order_session_rtt_probe {

class ProbeSampleExecutor {
 public:
  using CreateResult = Result<std::unique_ptr<ProbeSampleExecutor>>;

  [[nodiscard]] static CreateResult Create(PassiveOrderBuildResult gtc_passive,
                                           PassiveOrderBuildResult ioc_passive,
                                           ProbeSampleLocalIds ids) {
    CreateResult result;
    if (!gtc_passive.ok) {
      result.error =
          gtc_passive.error.empty()
              ? "gtc passive order build failed"
              : "gtc passive order build failed: " + gtc_passive.error;
      return result;
    }
    if (!ioc_passive.ok) {
      result.error =
          ioc_passive.error.empty()
              ? "ioc passive order build failed"
              : "ioc passive order build failed: " + ioc_passive.error;
      return result;
    }
    result.ok = true;
    result.value.reset(new ProbeSampleExecutor(std::move(gtc_passive),
                                               std::move(ioc_passive), ids));
    return result;
  }

  template <typename Session>
  [[nodiscard]] ProbeSampleTransition Start(Session& session) {
    const ProbeSampleAction action = flow_.Start();
    return Dispatch(session, action);
  }

  template <typename Session>
  [[nodiscard]] ProbeSampleTransition OnOrderResponse(
      Session& session, const gate::OrderResponse& response) {
    ProbeSampleTransition transition = flow_.OnOrderResponse(response);
    if (!transition.ok) {
      return transition;
    }
    if (transition.action == ProbeSampleAction::kNone ||
        transition.action == ProbeSampleAction::kFinish) {
      return transition;
    }
    return Dispatch(session, transition.action);
  }

  [[nodiscard]] const ProbeSampleStats& stats() const noexcept {
    return flow_.stats();
  }

 private:
  ProbeSampleExecutor(PassiveOrderBuildResult gtc_passive,
                      PassiveOrderBuildResult ioc_passive,
                      ProbeSampleLocalIds ids)
      : flow_(ids),
        gtc_place_(BuildGtcPlaceOrder(gtc_passive, ids)),
        gtc_cancel_(BuildGtcCancelOrder(gtc_place_)),
        ioc_place_(BuildIocPlaceOrder(ioc_passive, ids)),
        gtc_close_(BuildGtcCloseOrder(gtc_passive, ids)),
        ioc_close_(BuildIocCloseOrder(ioc_passive, ids)) {}

  template <typename Session>
  [[nodiscard]] ProbeSampleTransition Dispatch(Session& session,
                                               ProbeSampleAction action) {
    switch (action) {
      case ProbeSampleAction::kSubmitGtcPlace:
        return SubmitPlace(session, ProbeStage::kGtcPlace, gtc_place_);
      case ProbeSampleAction::kSubmitGtcCancel:
        return SubmitCancel(session, ProbeStage::kGtcCancel, gtc_cancel_);
      case ProbeSampleAction::kSubmitIocPlace:
        return SubmitPlace(session, ProbeStage::kIocPlace, ioc_place_);
      case ProbeSampleAction::kSubmitGtcClose:
        return SubmitPlace(session, ProbeStage::kGtcClose, gtc_close_);
      case ProbeSampleAction::kSubmitIocClose:
        return SubmitPlace(session, ProbeStage::kIocClose, ioc_close_);
      case ProbeSampleAction::kNone:
      case ProbeSampleAction::kFinish:
        return ProbeSampleTransition{.ok = true, .action = action};
      case ProbeSampleAction::kFail:
        return ProbeSampleTransition{.ok = false, .action = action};
    }
    return ProbeSampleTransition{.ok = false,
                                 .action = ProbeSampleAction::kFail,
                                 .error = "unknown sample action"};
  }

  template <typename Session>
  [[nodiscard]] ProbeSampleTransition SubmitPlace(Session& session,
                                                  ProbeStage stage,
                                                  const ProbeWireOrder& order) {
    const gate::OrderSendResult sent = session.PlaceOrder(order);
    ProbeSampleTransition transition = flow_.OnOrderSent(stage, sent);
    if (!transition.ok) {
      return transition;
    }
    transition.action = ProbeSampleAction::kNone;
    return transition;
  }

  template <typename Session>
  [[nodiscard]] ProbeSampleTransition SubmitCancel(
      Session& session, ProbeStage stage, const ProbeWireOrder& order) {
    const gate::OrderSendResult sent = session.CancelOrder(order);
    ProbeSampleTransition transition = flow_.OnOrderSent(stage, sent);
    if (!transition.ok) {
      return transition;
    }
    transition.action = ProbeSampleAction::kNone;
    return transition;
  }

  ProbeSampleFlow flow_;
  ProbeWireOrder gtc_place_;
  ProbeWireOrder gtc_cancel_;
  ProbeWireOrder ioc_place_;
  ProbeWireOrder gtc_close_;
  ProbeWireOrder ioc_close_;
};

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_EXECUTOR_H_
