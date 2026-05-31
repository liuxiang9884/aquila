#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_FLOW_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_FLOW_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "core/common/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_latency.h"
#include "exchange/gate/trading/order_types.h"
#include "tools/gate/order_session_rtt_probe/order_mode.h"
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

enum class ProbeStageStatus : std::uint8_t {
  kNotSubmitted,
  kSent,
  kAcked,
  kRejected,
  kSendFailed,
  kTimeout,
  kTerminalConfirmed,
};

struct ProbeStageCsvStats {
  std::uint64_t local_order_id{0};
  std::uint64_t request_sequence{0};
  std::int64_t request_send_local_ns{0};
  std::int64_t ack_receive_local_ns{0};
  std::int64_t ack_exchange_ns{0};
  std::int64_t ack_exchange_to_local_ns{0};
  std::int64_t ack_rtt_ns{-1};
  std::int64_t ts_write_complete_ns{0};
  std::int64_t ts_tx_sched_ns{0};
  std::int64_t ts_tx_software_ns{0};
  std::int64_t ts_tx_ack_ns{0};
  std::int64_t ts_rx_software_ns{0};
  std::int64_t ts_write_to_tx_software_ns{-1};
  std::int64_t ts_tx_software_to_tx_ack_ns{-1};
  std::int64_t ts_tx_ack_to_rx_software_ns{-1};
  std::int64_t ts_rx_software_to_ack_receive_ns{-1};
  std::int64_t response_receive_local_ns{0};
  std::int64_t response_exchange_ns{0};
  std::int64_t response_exchange_to_local_ns{0};
  std::int64_t response_rtt_ns{-1};
  bool has_terminal_feedback_kind{false};
  OrderFeedbackKind terminal_feedback_kind{OrderFeedbackKind::kAccepted};
};

struct ProbeSampleStats {
  ProbeStageCsvStats gtc_open_csv;
  ProbeStageCsvStats gtc_cancel_csv;
  ProbeStageCsvStats ioc_open_csv;
  ProbeStageCsvStats gtc_close_csv;
  ProbeStageCsvStats ioc_close_csv;
  std::uint64_t gtc_local_order_id{0};
  std::uint64_t ioc_local_order_id{0};
  std::uint64_t gtc_close_local_order_id{0};
  std::uint64_t ioc_close_local_order_id{0};
  std::int64_t gtc_place_request_send_local_ns{0};
  std::int64_t gtc_place_ack_receive_local_ns{0};
  std::int64_t gtc_place_ack_exchange_ns{0};
  std::int64_t gtc_place_ack_exchange_to_local_ns{0};
  std::int64_t gtc_place_ack_rtt_ns{-1};
  ProbeStageStatus gtc_place_status{ProbeStageStatus::kNotSubmitted};
  std::int64_t gtc_cancel_request_send_local_ns{0};
  std::int64_t gtc_cancel_ack_receive_local_ns{0};
  std::int64_t gtc_cancel_ack_exchange_ns{0};
  std::int64_t gtc_cancel_ack_exchange_to_local_ns{0};
  std::int64_t gtc_cancel_ack_rtt_ns{-1};
  ProbeStageStatus gtc_cancel_status{ProbeStageStatus::kNotSubmitted};
  std::int64_t ioc_place_request_send_local_ns{0};
  std::int64_t ioc_place_ack_receive_local_ns{0};
  std::int64_t ioc_place_ack_exchange_ns{0};
  std::int64_t ioc_place_ack_exchange_to_local_ns{0};
  std::int64_t ioc_place_ack_rtt_ns{-1};
  ProbeStageStatus ioc_place_status{ProbeStageStatus::kNotSubmitted};
  std::int64_t gtc_close_request_send_local_ns{0};
  std::int64_t gtc_close_ack_receive_local_ns{0};
  std::int64_t gtc_close_ack_exchange_ns{0};
  std::int64_t gtc_close_ack_exchange_to_local_ns{0};
  std::int64_t gtc_close_ack_rtt_ns{-1};
  ProbeStageStatus gtc_close_status{ProbeStageStatus::kNotSubmitted};
  std::int64_t ioc_close_request_send_local_ns{0};
  std::int64_t ioc_close_ack_receive_local_ns{0};
  std::int64_t ioc_close_ack_exchange_ns{0};
  std::int64_t ioc_close_ack_exchange_to_local_ns{0};
  std::int64_t ioc_close_ack_rtt_ns{-1};
  ProbeStageStatus ioc_close_status{ProbeStageStatus::kNotSubmitted};
  bool unexpected_fill{false};
  bool invalid_for_rtt_distribution{false};
  std::string invalid_reason;
};

class ProbeSampleFlow {
 public:
  explicit ProbeSampleFlow(
      ProbeSampleLocalIds ids,
      ProbeOrderMode order_mode = ProbeOrderMode::kIocAndGtc)
      : ids_(ids), order_mode_(order_mode) {}

  [[nodiscard]] ProbeSampleAction Start() noexcept {
    started_ = true;
    if (ProbeOrderModeUsesGtc(order_mode_)) {
      return ProbeSampleAction::kSubmitGtcPlace;
    }
    return ProbeSampleAction::kSubmitIocPlace;
  }

  [[nodiscard]] ProbeSampleTransition OnOrderSent(
      ProbeStage stage, const gate::OrderSendResult& sent) {
    ProbeStageSendState* state = MutableSendState(stage);
    ProbeStageStatus* status = MutableStatus(stage);
    if (state == nullptr || status == nullptr) {
      return Fail("unsupported probe stage");
    }
    if (sent.status != gate::OrderSendStatus::kOk) {
      *status = ProbeStageStatus::kSendFailed;
      return Fail("order send failed");
    }
    *state = ProbeStageSendState{.sent = true,
                                 .request_sequence = sent.request_sequence,
                                 .send_local_ns = sent.send_local_ns};
    *status = ProbeStageStatus::kSent;
    std::int64_t* request_send_local_ns = MutableRequestSendLocalNs(stage);
    if (request_send_local_ns != nullptr) {
      *request_send_local_ns = sent.send_local_ns;
    }
    std::uint64_t* local_order_id = MutableLocalOrderId(stage);
    if (local_order_id != nullptr) {
      *local_order_id = LocalOrderIdForStage(stage);
    }
    ProbeStageCsvStats* csv = MutableCsvStats(stage);
    if (csv != nullptr) {
      csv->local_order_id = LocalOrderIdForStage(stage);
      csv->request_sequence = sent.request_sequence;
      csv->request_send_local_ns = sent.send_local_ns;
    }
    return ProbeSampleTransition{.ok = true};
  }

  [[nodiscard]] ProbeSampleTransition OnOrderResponse(
      const gate::OrderResponse& response) {
    if (response.kind != gate::OrderResponseKind::kAck) {
      return RecordFinalResponse(response);
    }
    const ProbeStage stage = StageForRequestSequence(response.request_sequence);
    if (stage == ProbeStage::kIdle) {
      return ProbeSampleTransition{.ok = true};
    }
    const ProbeStageSendState* state = SendState(stage);
    if (state == nullptr || !state->sent) {
      return Fail("ack received before order send");
    }
    if (response.local_order_id != LocalOrderIdForStage(stage)) {
      return Fail("ack local_order_id does not match sample stage");
    }
    const std::int64_t rtt_ns =
        response.local_receive_ns - state->send_local_ns;
    if (rtt_ns < 0) {
      return Fail("negative ack rtt");
    }
    return RecordAckRtt(
        stage, response.local_receive_ns, response.exchange_ns,
        core::LatencyDeltaNs(response.local_receive_ns, response.exchange_ns),
        rtt_ns, response.socket_timestamps, response.socket_timestamp_stages);
  }

  [[nodiscard]] ProbeSampleTransition OnOrderFeedback(
      const OrderFeedbackEvent& feedback) {
    if (feedback.kind == OrderFeedbackKind::kContinuityLost) {
      return Fail("feedback continuity lost");
    }
    const ProbeStage stage = StageForLocalOrderId(feedback.local_order_id);
    if (stage == ProbeStage::kIdle) {
      return ProbeSampleTransition{.ok = true};
    }
    if (IsSafetyCloseStage(stage)) {
      return RecordSafetyCloseTerminalFeedback(stage, feedback);
    }
    if (IsFillFeedback(feedback)) {
      return RecordUnexpectedFill(stage, feedback);
    }
    if (stage == ProbeStage::kIocPlace &&
        feedback.kind == OrderFeedbackKind::kCancelled) {
      return RecordIocNoFillTerminalFeedback(feedback);
    }
    return ProbeSampleTransition{.ok = true};
  }

  [[nodiscard]] ProbeSampleTransition OnStageTimeout(ProbeStage stage) {
    ProbeStageStatus* status = MutableStatus(stage);
    if (status != nullptr) {
      *status = ProbeStageStatus::kTimeout;
    }
    if (stage == ProbeStage::kGtcClose || stage == ProbeStage::kIocClose) {
      MarkInvalid("safety close timeout", /*unexpected_fill=*/false);
      return Fail("safety close timeout");
    }
    if (stage == ProbeStage::kGtcPlace || stage == ProbeStage::kGtcCancel) {
      MarkInvalid("gtc timeout", /*unexpected_fill=*/false);
      if (!gtc_close_.sent) {
        return ProbeSampleTransition{
            .ok = true, .action = ProbeSampleAction::kSubmitGtcClose};
      }
      return ProbeSampleTransition{.ok = true};
    }
    if (stage == ProbeStage::kIocPlace) {
      MarkInvalid("ioc timeout", /*unexpected_fill=*/false);
      if (!ioc_close_.sent) {
        return ProbeSampleTransition{
            .ok = true, .action = ProbeSampleAction::kSubmitIocClose};
      }
      return ProbeSampleTransition{.ok = true};
    }
    return Fail("unsupported timeout stage");
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

  [[nodiscard]] ProbeStageStatus* MutableStatus(ProbeStage stage) noexcept {
    switch (stage) {
      case ProbeStage::kGtcPlace:
        return &stats_.gtc_place_status;
      case ProbeStage::kGtcCancel:
        return &stats_.gtc_cancel_status;
      case ProbeStage::kGtcClose:
        return &stats_.gtc_close_status;
      case ProbeStage::kIocPlace:
        return &stats_.ioc_place_status;
      case ProbeStage::kIocClose:
        return &stats_.ioc_close_status;
      case ProbeStage::kIdle:
        return nullptr;
    }
    return nullptr;
  }

  [[nodiscard]] ProbeStageCsvStats* MutableCsvStats(ProbeStage stage) noexcept {
    switch (stage) {
      case ProbeStage::kGtcPlace:
        return &stats_.gtc_open_csv;
      case ProbeStage::kGtcCancel:
        return &stats_.gtc_cancel_csv;
      case ProbeStage::kGtcClose:
        return &stats_.gtc_close_csv;
      case ProbeStage::kIocPlace:
        return &stats_.ioc_open_csv;
      case ProbeStage::kIocClose:
        return &stats_.ioc_close_csv;
      case ProbeStage::kIdle:
        return nullptr;
    }
    return nullptr;
  }

  [[nodiscard]] std::uint64_t* MutableLocalOrderId(ProbeStage stage) noexcept {
    switch (stage) {
      case ProbeStage::kGtcPlace:
      case ProbeStage::kGtcCancel:
        return &stats_.gtc_local_order_id;
      case ProbeStage::kGtcClose:
        return &stats_.gtc_close_local_order_id;
      case ProbeStage::kIocPlace:
        return &stats_.ioc_local_order_id;
      case ProbeStage::kIocClose:
        return &stats_.ioc_close_local_order_id;
      case ProbeStage::kIdle:
        return nullptr;
    }
    return nullptr;
  }

  [[nodiscard]] std::int64_t* MutableRequestSendLocalNs(
      ProbeStage stage) noexcept {
    switch (stage) {
      case ProbeStage::kGtcPlace:
        return &stats_.gtc_place_request_send_local_ns;
      case ProbeStage::kGtcCancel:
        return &stats_.gtc_cancel_request_send_local_ns;
      case ProbeStage::kGtcClose:
        return &stats_.gtc_close_request_send_local_ns;
      case ProbeStage::kIocPlace:
        return &stats_.ioc_place_request_send_local_ns;
      case ProbeStage::kIocClose:
        return &stats_.ioc_close_request_send_local_ns;
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

  [[nodiscard]] std::uint64_t LocalOrderIdForStage(
      ProbeStage stage) const noexcept {
    switch (stage) {
      case ProbeStage::kGtcPlace:
      case ProbeStage::kGtcCancel:
        return ids_.gtc_local_order_id;
      case ProbeStage::kGtcClose:
        return ids_.gtc_close_local_order_id;
      case ProbeStage::kIocPlace:
        return ids_.ioc_local_order_id;
      case ProbeStage::kIocClose:
        return ids_.ioc_close_local_order_id;
      case ProbeStage::kIdle:
        return 0;
    }
    return 0;
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

  [[nodiscard]] ProbeStage StageForLocalOrderId(
      std::uint64_t local_order_id) const noexcept {
    if (local_order_id == ids_.gtc_local_order_id) {
      return ProbeStage::kGtcPlace;
    }
    if (local_order_id == ids_.gtc_close_local_order_id) {
      return ProbeStage::kGtcClose;
    }
    if (local_order_id == ids_.ioc_local_order_id) {
      return ProbeStage::kIocPlace;
    }
    if (local_order_id == ids_.ioc_close_local_order_id) {
      return ProbeStage::kIocClose;
    }
    return ProbeStage::kIdle;
  }

  [[nodiscard]] ProbeSampleTransition RecordAckRtt(
      ProbeStage stage, std::int64_t ack_receive_local_ns,
      std::int64_t ack_exchange_ns, std::int64_t ack_exchange_to_local_ns,
      std::int64_t rtt_ns,
      const websocket::SocketTimestampingSnapshot& socket_timestamps,
      const websocket::SocketTimestampingStages& socket_timestamp_stages) {
    ProbeStageCsvStats* csv = MutableCsvStats(stage);
    if (csv != nullptr) {
      csv->ack_receive_local_ns = ack_receive_local_ns;
      csv->ack_exchange_ns = ack_exchange_ns;
      csv->ack_exchange_to_local_ns = ack_exchange_to_local_ns;
      csv->ack_rtt_ns = rtt_ns;
      csv->ts_write_complete_ns = socket_timestamps.write_complete_ns;
      csv->ts_tx_sched_ns = socket_timestamps.tx_sched_ns;
      csv->ts_tx_software_ns = socket_timestamps.tx_software_ns;
      csv->ts_tx_ack_ns = socket_timestamps.tx_ack_ns;
      csv->ts_rx_software_ns = socket_timestamps.rx_software_ns;
      csv->ts_write_to_tx_software_ns =
          socket_timestamp_stages.write_complete_to_tx_software_ns;
      csv->ts_tx_software_to_tx_ack_ns =
          socket_timestamp_stages.tx_software_to_tx_ack_ns;
      csv->ts_tx_ack_to_rx_software_ns =
          socket_timestamp_stages.tx_ack_to_rx_software_ns;
      csv->ts_rx_software_to_ack_receive_ns =
          socket_timestamp_stages.rx_software_to_ack_receive_ns;
    }
    switch (stage) {
      case ProbeStage::kGtcPlace:
        stats_.gtc_place_ack_receive_local_ns = ack_receive_local_ns;
        stats_.gtc_place_ack_exchange_ns = ack_exchange_ns;
        stats_.gtc_place_ack_exchange_to_local_ns = ack_exchange_to_local_ns;
        stats_.gtc_place_ack_rtt_ns = rtt_ns;
        stats_.gtc_place_status = ProbeStageStatus::kAcked;
        return ProbeSampleTransition{
            .ok = true, .action = ProbeSampleAction::kSubmitGtcCancel};
      case ProbeStage::kGtcCancel:
        stats_.gtc_cancel_ack_receive_local_ns = ack_receive_local_ns;
        stats_.gtc_cancel_ack_exchange_ns = ack_exchange_ns;
        stats_.gtc_cancel_ack_exchange_to_local_ns = ack_exchange_to_local_ns;
        stats_.gtc_cancel_ack_rtt_ns = rtt_ns;
        stats_.gtc_cancel_status = ProbeStageStatus::kAcked;
        if (!ProbeOrderModeUsesIoc(order_mode_)) {
          return ProbeSampleTransition{.ok = true,
                                       .action = ProbeSampleAction::kFinish};
        }
        return ProbeSampleTransition{
            .ok = true, .action = ProbeSampleAction::kSubmitIocPlace};
      case ProbeStage::kIocPlace:
        stats_.ioc_place_ack_receive_local_ns = ack_receive_local_ns;
        stats_.ioc_place_ack_exchange_ns = ack_exchange_ns;
        stats_.ioc_place_ack_exchange_to_local_ns = ack_exchange_to_local_ns;
        stats_.ioc_place_ack_rtt_ns = rtt_ns;
        stats_.ioc_place_status = ProbeStageStatus::kAcked;
        if (order_mode_ == ProbeOrderMode::kIoc) {
          if (ioc_place_terminal_confirmed_) {
            stats_.ioc_place_status = ProbeStageStatus::kTerminalConfirmed;
            return ProbeSampleTransition{.ok = true,
                                         .action = ProbeSampleAction::kFinish};
          }
          return ProbeSampleTransition{.ok = true};
        }
        return ProbeSampleTransition{
            .ok = true, .action = ProbeSampleAction::kSubmitIocClose};
      case ProbeStage::kGtcClose:
        stats_.gtc_close_ack_receive_local_ns = ack_receive_local_ns;
        stats_.gtc_close_ack_exchange_ns = ack_exchange_ns;
        stats_.gtc_close_ack_exchange_to_local_ns = ack_exchange_to_local_ns;
        stats_.gtc_close_ack_rtt_ns = rtt_ns;
        stats_.gtc_close_status = ProbeStageStatus::kAcked;
        return ProbeSampleTransition{.ok = true};
      case ProbeStage::kIocClose:
        stats_.ioc_close_ack_receive_local_ns = ack_receive_local_ns;
        stats_.ioc_close_ack_exchange_ns = ack_exchange_ns;
        stats_.ioc_close_ack_exchange_to_local_ns = ack_exchange_to_local_ns;
        stats_.ioc_close_ack_rtt_ns = rtt_ns;
        stats_.ioc_close_status = ProbeStageStatus::kAcked;
        return ProbeSampleTransition{.ok = true};
      case ProbeStage::kIdle:
        break;
    }
    return ProbeSampleTransition{.ok = true};
  }

  [[nodiscard]] ProbeSampleTransition RecordFinalResponse(
      const gate::OrderResponse& response) {
    const ProbeStage stage = StageForRequestSequence(response.request_sequence);
    if (stage == ProbeStage::kIdle) {
      return ProbeSampleTransition{.ok = true};
    }
    const ProbeStageSendState* state = SendState(stage);
    if (state == nullptr || !state->sent) {
      return Fail("final response received before order send");
    }
    if (response.local_order_id != LocalOrderIdForStage(stage)) {
      return Fail("final response local_order_id does not match sample stage");
    }
    RecordFinalResponseTiming(stage, response, *state);
    switch (response.kind) {
      case gate::OrderResponseKind::kAck:
        break;
      case gate::OrderResponseKind::kAccepted:
      case gate::OrderResponseKind::kCancelAccepted:
        return ProbeSampleTransition{.ok = true};
      case gate::OrderResponseKind::kCancelRejected:
        return RecordCancelRejected(stage);
      case gate::OrderResponseKind::kRejected:
        return RecordRejected(stage);
    }
    return ProbeSampleTransition{.ok = true};
  }

  [[nodiscard]] ProbeSampleTransition RecordCancelRejected(ProbeStage stage) {
    if (ShouldSubmitGtcSafetyClose(SafetyCloseInput{
            .stage = stage,
            .response_kind = gate::OrderResponseKind::kCancelRejected,
        })) {
      stats_.gtc_cancel_status = ProbeStageStatus::kRejected;
      MarkInvalid("gtc cancel rejected", /*unexpected_fill=*/false);
      if (!gtc_close_.sent) {
        return ProbeSampleTransition{
            .ok = true, .action = ProbeSampleAction::kSubmitGtcClose};
      }
      return ProbeSampleTransition{.ok = true};
    }
    ProbeStageStatus* status = MutableStatus(stage);
    if (status != nullptr) {
      *status = ProbeStageStatus::kRejected;
    }
    return Fail("unexpected cancel rejected");
  }

  [[nodiscard]] ProbeSampleTransition RecordRejected(ProbeStage stage) {
    ProbeStageStatus* status = MutableStatus(stage);
    if (status != nullptr) {
      *status = ProbeStageStatus::kRejected;
    }
    if (stage == ProbeStage::kGtcClose || stage == ProbeStage::kIocClose) {
      return Fail("safety close rejected without flat confirmation");
    }
    return Fail("order rejected");
  }

  [[nodiscard]] static bool IsSafetyCloseStage(ProbeStage stage) noexcept {
    return stage == ProbeStage::kGtcClose || stage == ProbeStage::kIocClose;
  }

  [[nodiscard]] static bool IsFillFeedback(
      const OrderFeedbackEvent& feedback) noexcept {
    return feedback.kind == OrderFeedbackKind::kPartialFilled ||
           feedback.kind == OrderFeedbackKind::kFilled ||
           feedback.cumulative_filled_quantity > 0.0;
  }

  void RecordFinalResponseTiming(ProbeStage stage,
                                 const gate::OrderResponse& response,
                                 const ProbeStageSendState& state) {
    ProbeStageCsvStats* csv = MutableCsvStats(stage);
    if (csv == nullptr) {
      return;
    }
    csv->response_receive_local_ns = response.local_receive_ns;
    csv->response_exchange_ns = response.exchange_ns;
    csv->response_exchange_to_local_ns =
        core::LatencyDeltaNs(response.local_receive_ns, response.exchange_ns);
    csv->response_rtt_ns =
        core::LatencyDeltaNs(response.local_receive_ns, state.send_local_ns);
  }

  void RecordTerminalFeedbackKind(ProbeStage stage,
                                  const OrderFeedbackEvent& feedback) {
    ProbeStageCsvStats* csv = MutableCsvStats(stage);
    if (csv == nullptr) {
      return;
    }
    csv->has_terminal_feedback_kind = true;
    csv->terminal_feedback_kind = feedback.kind;
  }

  [[nodiscard]] ProbeSampleTransition RecordUnexpectedFill(
      ProbeStage stage, const OrderFeedbackEvent& feedback) {
    RecordTerminalFeedbackKind(stage, feedback);
    MarkInvalid("feedback fill", /*unexpected_fill=*/true);
    if (stage == ProbeStage::kGtcPlace && !gtc_close_.sent) {
      return ProbeSampleTransition{
          .ok = true, .action = ProbeSampleAction::kSubmitGtcClose};
    }
    if (stage == ProbeStage::kIocPlace && !ioc_close_.sent) {
      return ProbeSampleTransition{
          .ok = true, .action = ProbeSampleAction::kSubmitIocClose};
    }
    return ProbeSampleTransition{.ok = true};
  }

  [[nodiscard]] ProbeSampleTransition RecordIocNoFillTerminalFeedback(
      const OrderFeedbackEvent& feedback) {
    RecordTerminalFeedbackKind(ProbeStage::kIocPlace, feedback);
    ioc_place_terminal_confirmed_ = true;
    if (stats_.ioc_place_ack_rtt_ns < 0) {
      return ProbeSampleTransition{.ok = true};
    }
    stats_.ioc_place_status = ProbeStageStatus::kTerminalConfirmed;
    return ProbeSampleTransition{.ok = true,
                                 .action = ProbeSampleAction::kFinish};
  }

  [[nodiscard]] ProbeSampleTransition RecordSafetyCloseTerminalFeedback(
      ProbeStage stage, const OrderFeedbackEvent& feedback) {
    RecordTerminalFeedbackKind(stage, feedback);
    ProbeStageStatus* status = MutableStatus(stage);
    if (feedback.kind == OrderFeedbackKind::kFilled) {
      if (status != nullptr) {
        *status = ProbeStageStatus::kTerminalConfirmed;
      }
      return ProbeSampleTransition{.ok = true,
                                   .action = ProbeSampleAction::kFinish};
    }
    if (feedback.kind == OrderFeedbackKind::kRejected ||
        feedback.kind == OrderFeedbackKind::kCancelled) {
      if (status != nullptr) {
        *status = ProbeStageStatus::kRejected;
      }
      return Fail("safety close terminal feedback does not prove flat");
    }
    return ProbeSampleTransition{.ok = true};
  }

  void MarkInvalid(std::string_view reason, bool unexpected_fill) {
    stats_.unexpected_fill = stats_.unexpected_fill || unexpected_fill;
    stats_.invalid_for_rtt_distribution = true;
    if (stats_.invalid_reason.empty()) {
      stats_.invalid_reason.assign(reason.data(), reason.size());
      return;
    }
    stats_.invalid_reason.append("; ");
    stats_.invalid_reason.append(reason.data(), reason.size());
  }

  ProbeSampleLocalIds ids_;
  ProbeOrderMode order_mode_{ProbeOrderMode::kIocAndGtc};
  bool started_{false};
  ProbeSampleStats stats_;
  ProbeStageSendState gtc_place_;
  ProbeStageSendState gtc_cancel_;
  ProbeStageSendState gtc_close_;
  ProbeStageSendState ioc_place_;
  ProbeStageSendState ioc_close_;
  bool ioc_place_terminal_confirmed_{false};
};

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_FLOW_H_
