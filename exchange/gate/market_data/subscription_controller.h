#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_SUBSCRIPTION_CONTROLLER_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_SUBSCRIPTION_CONTROLLER_H_

#include <cstdint>

#include "core/websocket/types.h"

namespace aquila::gate {

enum class SubscriptionState : std::uint8_t {
  kIdle = 0,
  kSubscribeSent,
  kSubscribed,
  kUnsubscribeSent,
  kUnsubscribed,
  kRejected,
};

class BookTickerSubscriptionController {
 public:
  [[nodiscard]] bool OnConnectionPhase(
      websocket::ConnectionPhase phase) noexcept {
    switch (phase) {
      case websocket::ConnectionPhase::kActive:
        connection_active_ = true;
        return CanSendSubscribe();
      case websocket::ConnectionPhase::kDisconnected:
      case websocket::ConnectionPhase::kReconnectBackoff:
      case websocket::ConnectionPhase::kClosing:
      case websocket::ConnectionPhase::kClosed:
        connection_active_ = false;
        subscription_sent_for_connection_ = false;
        if (subscription_state_ == SubscriptionState::kSubscribeSent ||
            subscription_state_ == SubscriptionState::kSubscribed) {
          subscription_state_ = SubscriptionState::kIdle;
        }
        return false;
      case websocket::ConnectionPhase::kResolving:
      case websocket::ConnectionPhase::kTcpConnecting:
      case websocket::ConnectionPhase::kTlsHandshaking:
      case websocket::ConnectionPhase::kWsHandshaking:
      case websocket::ConnectionPhase::kDegraded:
        return false;
    }
    return false;
  }

  void RecordSubscribeAttempt(websocket::SendStatus status) noexcept {
    subscribe_status_ = status;
    subscription_sent_for_connection_ = status == websocket::SendStatus::kOk;
    if (status == websocket::SendStatus::kOk) {
      subscription_state_ = SubscriptionState::kSubscribeSent;
    }
  }

  void RecordUnsubscribeAttempt(websocket::SendStatus status) noexcept {
    unsubscribe_status_ = status;
    if (status == websocket::SendStatus::kOk) {
      subscription_state_ = SubscriptionState::kUnsubscribeSent;
    }
  }

  void MarkSubscribeAccepted() noexcept {
    subscription_state_ = SubscriptionState::kSubscribed;
  }

  void MarkSubscribeRejected() noexcept {
    subscription_state_ = SubscriptionState::kRejected;
  }

  void MarkUnsubscribeAccepted() noexcept {
    subscription_state_ = SubscriptionState::kUnsubscribed;
  }

  void MarkUnsubscribeRejected() noexcept {
    subscription_state_ = SubscriptionState::kRejected;
  }

  [[nodiscard]] bool CanRetrySubscribe() const noexcept {
    return connection_active_ && CanSendSubscribe();
  }

  [[nodiscard]] bool connection_active() const noexcept {
    return connection_active_;
  }

  [[nodiscard]] SubscriptionState subscription_state() const noexcept {
    return subscription_state_;
  }

  [[nodiscard]] websocket::SendStatus subscribe_status() const noexcept {
    return subscribe_status_;
  }

  [[nodiscard]] websocket::SendStatus unsubscribe_status() const noexcept {
    return unsubscribe_status_;
  }

 private:
  [[nodiscard]] bool CanSendSubscribe() const noexcept {
    return !subscription_sent_for_connection_ &&
           subscription_state_ != SubscriptionState::kRejected;
  }

  SubscriptionState subscription_state_{SubscriptionState::kIdle};
  websocket::SendStatus subscribe_status_{
      websocket::SendStatus::kWriteUnavailable};
  websocket::SendStatus unsubscribe_status_{
      websocket::SendStatus::kWriteUnavailable};
  bool subscription_sent_for_connection_{false};
  bool connection_active_{false};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_SUBSCRIPTION_CONTROLLER_H_
