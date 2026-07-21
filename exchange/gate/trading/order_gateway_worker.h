#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_GATEWAY_WORKER_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_GATEWAY_WORKER_H_

#include <cstdint>

#include "absl/container/flat_hash_map.h"
#include "core/trading/order_gateway_shm.h"
#include "core/trading/order_types.h"
#include "core/websocket/runtime_clock.h"
#include "exchange/gate/trading/order_session_runtime_adapter.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::gate {

struct OrderGatewayRequestMetadata {
  std::uint64_t command_seq{0};
  std::uint64_t parent_id{0};
  std::uint64_t group_id{0};
  std::int64_t worker_dequeue_ns{0};
  std::int64_t request_send_local_ns{0};
};

[[nodiscard]] inline core::OrderGatewayCommandRejectReason
ToOrderGatewayRejectReason(OrderSendStatus status) noexcept {
  using Reason = core::OrderGatewayCommandRejectReason;
  switch (status) {
    case OrderSendStatus::kOk:
      return Reason::kNone;
    case OrderSendStatus::kNotLoggedIn:
      return Reason::kSessionNotReady;
    case OrderSendStatus::kNotActive:
      return Reason::kSessionNotActive;
    case OrderSendStatus::kInflightFull:
      return Reason::kInflightFull;
    case OrderSendStatus::kEncodeBufferTooSmall:
    case OrderSendStatus::kInvalidLocalOrderId:
    case OrderSendStatus::kInvalidQuantityText:
    case OrderSendStatus::kSignatureFailed:
      return Reason::kEncodeFailed;
    case OrderSendStatus::kInvalidRoute:
      return Reason::kInvalidCommand;
    case OrderSendStatus::kUnsupportedOrderType:
      return Reason::kUnsupportedOrderType;
    case OrderSendStatus::kNoPreparedWriteSlot:
      return Reason::kNoPreparedWriteSlot;
    case OrderSendStatus::kWriteUnavailable:
      return Reason::kWriteUnavailable;
  }
  return Reason::kInvalidCommand;
}

class OrderGatewayWorkerPublisher {
 public:
  OrderGatewayWorkerPublisher() noexcept = default;
  OrderGatewayWorkerPublisher(std::uint16_t route_id,
                              core::OrderGatewayEventQueue event_queue,
                              core::OrderGatewayShmHeader* header = nullptr)
      : route_id_(route_id), event_queue_(event_queue), header_(header) {}

  [[nodiscard]] bool PublishCommandRejected(
      const core::OrderGatewayCommand& command,
      core::OrderGatewayCommandRejectReason reject_reason,
      std::uint64_t request_sequence = 0, std::uint64_t encoded_request_id = 0,
      std::int64_t request_send_local_ns = 0,
      std::int64_t worker_dequeue_ns = 0) noexcept {
    core::OrderGatewayEvent event{};
    event.event_seq = NextEventSeq();
    event.command_seq = command.command_seq;
    event.parent_id = core::OrderGatewayCommandParentId(command);
    event.group_id = core::OrderGatewayCommandGroupId(command);
    event.local_order_id = core::OrderGatewayCommandLocalOrderId(command);
    event.exchange_order_id = core::OrderGatewayCommandExchangeOrderId(command);
    event.request_sequence = request_sequence;
    event.encoded_request_id = encoded_request_id;
    event.worker_dequeue_ns = worker_dequeue_ns;
    event.request_send_local_ns = request_send_local_ns;
    event.worker_event_enqueue_ns = NowNs();
    event.route_id = route_id_;
    event.kind = core::OrderGatewayEventKind::kCommandRejected;
    event.command_kind = command.kind;
    event.response_kind = core::OrderResponseKind::kRejected;
    event.reject_reason = reject_reason;
    return Publish(event);
  }

  [[nodiscard]] bool PublishStopped() noexcept {
    core::OrderGatewayEvent event{};
    event.event_seq = NextEventSeq();
    event.route_id = route_id_;
    event.kind = core::OrderGatewayEventKind::kStopped;
    event.worker_event_enqueue_ns = NowNs();
    request_metadata_.clear();
    StoreRouteState(core::OrderGatewayRouteState::kStopped);
    return Publish(event);
  }

  void OnOrderSessionLoginReady() noexcept {
    core::OrderGatewayEvent event{};
    event.event_seq = NextEventSeq();
    event.route_id = route_id_;
    event.kind = core::OrderGatewayEventKind::kReady;
    event.ready = 1;
    event.worker_event_enqueue_ns = NowNs();
    StoreRouteState(core::OrderGatewayRouteState::kReady);
    (void)Publish(event);
  }

  void OnOrderSessionLoginNotReady() noexcept {
    core::OrderGatewayEvent event{};
    event.event_seq = NextEventSeq();
    event.route_id = route_id_;
    event.kind = core::OrderGatewayEventKind::kNotReady;
    event.ready = 0;
    event.worker_event_enqueue_ns = NowNs();
    request_metadata_.clear();
    StoreRouteState(core::OrderGatewayRouteState::kNotReady);
    (void)Publish(event);
  }

  void OnOrderResponse(const gate::OrderResponse& response) noexcept {
    core::OrderGatewayEvent event{};
    event.event_seq = NextEventSeq();
    const auto metadata = request_metadata_.find(response.request_sequence);
    if (metadata != request_metadata_.end()) {
      event.command_seq = metadata->second.command_seq;
      event.parent_id = metadata->second.parent_id;
      event.group_id = metadata->second.group_id;
      event.worker_dequeue_ns = metadata->second.worker_dequeue_ns;
      event.request_send_local_ns = metadata->second.request_send_local_ns;
      if (response.kind != gate::OrderResponseKind::kAck) {
        request_metadata_.erase(metadata);
      }
    }
    event.local_order_id = response.local_order_id;
    event.exchange_order_id = response.exchange_order_id;
    event.request_sequence = response.request_sequence;
    event.local_receive_ns = response.local_receive_ns;
    event.exchange_ns = response.exchange_ns;
    event.exchange_request_ingress_ns = response.exchange_request_ingress_ns;
    event.exchange_response_egress_ns = response.exchange_response_egress_ns;
    event.exchange_process_ns = response.exchange_process_ns;
    event.route_id = route_id_;
    event.http_status = response.http_status;
    event.kind = core::OrderGatewayEventKind::kOrderResponse;
    event.response_kind = gate::ToCoreOrderResponseKind(response);
    event.worker_event_enqueue_ns = NowNs();
    (void)Publish(event);
  }

  [[nodiscard]] bool TrackSentCommand(const core::OrderGatewayCommand& command,
                                      const gate::OrderSendResult& sent,
                                      std::int64_t worker_dequeue_ns) noexcept {
    if (sent.request_sequence == 0) {
      return true;
    }
    try {
      request_metadata_.insert_or_assign(
          sent.request_sequence,
          OrderGatewayRequestMetadata{
              .command_seq = command.command_seq,
              .parent_id = core::OrderGatewayCommandParentId(command),
              .group_id = core::OrderGatewayCommandGroupId(command),
              .worker_dequeue_ns = worker_dequeue_ns,
              .request_send_local_ns = sent.send_local_ns,
          });
    } catch (...) {
      event_queue_failed_ = true;
      StoreRouteState(core::OrderGatewayRouteState::kStopped);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool event_queue_failed() const noexcept {
    return event_queue_failed_;
  }

 private:
  [[nodiscard]] std::uint64_t NextEventSeq() noexcept {
    return ++event_seq_;
  }

  [[nodiscard]] static std::int64_t NowNs() noexcept {
    return static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
  }

  [[nodiscard]] bool Publish(const core::OrderGatewayEvent& event) noexcept {
    if (!event_queue_.TryPush(event)) {
      event_queue_failed_ = true;
      StoreRouteState(core::OrderGatewayRouteState::kStopped);
      return false;
    }
    return true;
  }

  void StoreRouteState(core::OrderGatewayRouteState state) noexcept {
    if (header_ == nullptr) {
      return;
    }
    core::StoreOrderGatewayRouteState(*header_, route_id_, state);
  }

  std::uint16_t route_id_{0};
  core::OrderGatewayEventQueue event_queue_{};
  core::OrderGatewayShmHeader* header_{nullptr};
  std::uint64_t event_seq_{0};
  bool event_queue_failed_{false};
  absl::flat_hash_map<std::uint64_t, OrderGatewayRequestMetadata>
      request_metadata_;
};

class OrderGatewaySessionEventHandler {
 public:
  explicit OrderGatewaySessionEventHandler(
      OrderGatewayWorkerPublisher& publisher) noexcept
      : publisher_(&publisher) {}

  void OnOrderSessionLoginReady() noexcept {
    publisher_->OnOrderSessionLoginReady();
  }

  void OnOrderSessionLoginNotReady() noexcept {
    publisher_->OnOrderSessionLoginNotReady();
  }

  void OnOrderResponse(const gate::OrderResponse& response) noexcept {
    publisher_->OnOrderResponse(response);
  }

 private:
  OrderGatewayWorkerPublisher* publisher_{nullptr};
};

template <typename SessionT>
class OrderGatewayCommandWorker {
 public:
  OrderGatewayCommandWorker(std::uint16_t route_id,
                            core::OrderGatewayCommandQueue command_queue,
                            SessionT& session,
                            OrderGatewayWorkerPublisher& publisher) noexcept
      : route_id_(route_id),
        command_queue_(command_queue),
        session_(&session),
        publisher_(&publisher) {}

  [[nodiscard]] bool PollOnce() noexcept {
    if (stopped_ || publisher_->event_queue_failed()) {
      stopped_ = true;
      return false;
    }
    core::OrderGatewayCommand command{};
    if (!command_queue_.TryPop(&command)) {
      return false;
    }
    Dispatch(command, NowNs());
    return true;
  }

  [[nodiscard]] std::uint64_t Drain(std::uint64_t max_commands) noexcept {
    std::uint64_t drained = 0;
    while (drained < max_commands && PollOnce()) {
      ++drained;
    }
    return drained;
  }

  [[nodiscard]] bool stopped() const noexcept {
    return stopped_;
  }

 private:
  void Dispatch(const core::OrderGatewayCommand& command,
                std::int64_t worker_dequeue_ns) noexcept {
    if (command.kind == core::OrderGatewayCommandKind::kStop) {
      stopped_ = true;
      (void)publisher_->PublishStopped();
      return;
    }
    if (command.kind != core::OrderGatewayCommandKind::kStop &&
        core::OrderGatewayCommandRouteId(command) != route_id_) {
      (void)publisher_->PublishCommandRejected(
          command, core::OrderGatewayCommandRejectReason::kInvalidCommand, 0, 0,
          0, worker_dequeue_ns);
      StopIfPublisherFailed();
      return;
    }

    switch (command.kind) {
      case core::OrderGatewayCommandKind::kPlace:
        Place(command, worker_dequeue_ns);
        return;
      case core::OrderGatewayCommandKind::kCancel:
        Cancel(command, worker_dequeue_ns);
        return;
      case core::OrderGatewayCommandKind::kCacheExchangeOrderId:
        session_->CacheExchangeOrderId(
            command.payload.order_id.local_order_id,
            command.payload.order_id.exchange_order_id);
        return;
      case core::OrderGatewayCommandKind::kForgetExchangeOrderId:
        session_->ForgetExchangeOrderId(
            command.payload.order_id.local_order_id);
        return;
      case core::OrderGatewayCommandKind::kNone:
        break;
      case core::OrderGatewayCommandKind::kStop:
        return;
    }
    (void)publisher_->PublishCommandRejected(
        command, core::OrderGatewayCommandRejectReason::kInvalidCommand, 0, 0,
        0, worker_dequeue_ns);
    StopIfPublisherFailed();
  }

  void Place(const core::OrderGatewayCommand& command,
             std::int64_t worker_dequeue_ns) noexcept {
    const gate::OrderSendResult sent =
        session_->PlaceOrder(command.payload.place);
    if (sent.status != gate::OrderSendStatus::kOk) {
      (void)publisher_->PublishCommandRejected(
          command, ToOrderGatewayRejectReason(sent.status),
          sent.request_sequence, sent.encoded_request_id, sent.send_local_ns,
          worker_dequeue_ns);
      StopIfPublisherFailed();
      return;
    }
    if (!publisher_->TrackSentCommand(command, sent, worker_dequeue_ns)) {
      StopIfPublisherFailed();
    }
  }

  void Cancel(const core::OrderGatewayCommand& command,
              std::int64_t worker_dequeue_ns) noexcept {
    const gate::OrderSendResult sent =
        session_->CancelOrder(command.payload.cancel);
    if (sent.status != gate::OrderSendStatus::kOk) {
      (void)publisher_->PublishCommandRejected(
          command, ToOrderGatewayRejectReason(sent.status),
          sent.request_sequence, sent.encoded_request_id, sent.send_local_ns,
          worker_dequeue_ns);
      StopIfPublisherFailed();
      return;
    }
    if (!publisher_->TrackSentCommand(command, sent, worker_dequeue_ns)) {
      StopIfPublisherFailed();
    }
  }

  void StopIfPublisherFailed() noexcept {
    if (publisher_->event_queue_failed()) {
      stopped_ = true;
    }
  }

  [[nodiscard]] static std::int64_t NowNs() noexcept {
    return static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
  }

  std::uint16_t route_id_{0};
  core::OrderGatewayCommandQueue command_queue_{};
  SessionT* session_{nullptr};
  OrderGatewayWorkerPublisher* publisher_{nullptr};
  bool stopped_{false};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_GATEWAY_WORKER_H_
