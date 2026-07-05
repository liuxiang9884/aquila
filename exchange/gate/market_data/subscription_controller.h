#ifndef AQUILA_EXCHANGE_GATE_MARKET_DATA_SUBSCRIPTION_CONTROLLER_H_
#define AQUILA_EXCHANGE_GATE_MARKET_DATA_SUBSCRIPTION_CONTROLLER_H_

#include <cstdint>

#include "core/websocket/types.h"
#include "exchange/gate/market_data/data_session_config.h"

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
  BookTickerSubscriptionController() = default;

  explicit BookTickerSubscriptionController(DataSessionFeeds feeds) noexcept
      : feeds_(feeds) {}

  void ConfigureFeeds(DataSessionFeeds feeds) noexcept {
    feeds_ = feeds;
    ResetSubscribeSends();
    ResetAcks();
  }

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
        ResetSubscribeSends();
        ResetAcks();
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

  void RecordBookTickerSubscribeAttempt(websocket::SendStatus status) noexcept {
    RecordFeedSubscribeAttempt(status, &book_ticker_subscribe_sent_);
  }

  void RecordTradeSubscribeAttempt(websocket::SendStatus status) noexcept {
    RecordFeedSubscribeAttempt(status, &trade_subscribe_sent_);
  }

  void RecordSubscribeAttempt(websocket::SendStatus status) noexcept {
    RecordBookTickerSubscribeAttempt(status);
  }

  void RecordUnsubscribeAttempt(websocket::SendStatus status) noexcept {
    unsubscribe_status_ = status;
    if (status == websocket::SendStatus::kOk) {
      ResetUnsubscribeAcks();
      subscription_state_ = SubscriptionState::kUnsubscribeSent;
    }
  }

  void MarkBookTickerSubscribeAccepted() noexcept {
    book_ticker_subscribe_accepted_ = true;
    if (AllSubscribeAcksReceived()) {
      subscription_state_ = SubscriptionState::kSubscribed;
    }
  }

  void MarkTradeSubscribeAccepted() noexcept {
    trade_subscribe_accepted_ = true;
    if (AllSubscribeAcksReceived()) {
      subscription_state_ = SubscriptionState::kSubscribed;
    }
  }

  void MarkSubscribeRejected() noexcept {
    subscription_state_ = SubscriptionState::kRejected;
  }

  void MarkBookTickerUnsubscribeAccepted() noexcept {
    book_ticker_unsubscribe_accepted_ = true;
    if (AllUnsubscribeAcksReceived()) {
      subscription_state_ = SubscriptionState::kUnsubscribed;
    }
  }

  void MarkTradeUnsubscribeAccepted() noexcept {
    trade_unsubscribe_accepted_ = true;
    if (AllUnsubscribeAcksReceived()) {
      subscription_state_ = SubscriptionState::kUnsubscribed;
    }
  }

  void MarkUnsubscribeRejected() noexcept {
    subscription_state_ = SubscriptionState::kRejected;
  }

  [[nodiscard]] bool CanRetrySubscribe() const noexcept {
    return connection_active_ && CanSendSubscribe();
  }

  [[nodiscard]] bool ShouldSendBookTickerSubscribe() const noexcept {
    return feeds_.book_ticker && !book_ticker_subscribe_sent_ &&
           subscription_state_ != SubscriptionState::kRejected;
  }

  [[nodiscard]] bool ShouldSendTradeSubscribe() const noexcept {
    return feeds_.trade && !trade_subscribe_sent_ &&
           subscription_state_ != SubscriptionState::kRejected;
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
  void ResetAcks() noexcept {
    ResetSubscribeAcks();
    ResetUnsubscribeAcks();
  }

  void RecordFeedSubscribeAttempt(websocket::SendStatus status,
                                  bool* feed_sent) noexcept {
    subscribe_status_ = status;
    if (status == websocket::SendStatus::kOk) {
      if (!AnySubscribeSent()) {
        ResetSubscribeAcks();
      }
      *feed_sent = true;
      subscription_state_ = SubscriptionState::kSubscribeSent;
      return;
    }
    if (!AnySubscribeSent()) {
      subscription_state_ = SubscriptionState::kIdle;
    }
  }

  void ResetSubscribeAcks() noexcept {
    book_ticker_subscribe_accepted_ = false;
    trade_subscribe_accepted_ = false;
  }

  void ResetSubscribeSends() noexcept {
    book_ticker_subscribe_sent_ = false;
    trade_subscribe_sent_ = false;
  }

  void ResetUnsubscribeAcks() noexcept {
    book_ticker_unsubscribe_accepted_ = false;
    trade_unsubscribe_accepted_ = false;
  }

  [[nodiscard]] bool AllSubscribeAcksReceived() const noexcept {
    return (!feeds_.book_ticker || book_ticker_subscribe_accepted_) &&
           (!feeds_.trade || trade_subscribe_accepted_);
  }

  [[nodiscard]] bool AllUnsubscribeAcksReceived() const noexcept {
    return (!feeds_.book_ticker || book_ticker_unsubscribe_accepted_) &&
           (!feeds_.trade || trade_unsubscribe_accepted_);
  }

  [[nodiscard]] bool AnySubscribeSent() const noexcept {
    return book_ticker_subscribe_sent_ || trade_subscribe_sent_;
  }

  [[nodiscard]] bool CanSendSubscribe() const noexcept {
    return ShouldSendBookTickerSubscribe() || ShouldSendTradeSubscribe();
  }

  DataSessionFeeds feeds_{};
  SubscriptionState subscription_state_{SubscriptionState::kIdle};
  websocket::SendStatus subscribe_status_{
      websocket::SendStatus::kWriteUnavailable};
  websocket::SendStatus unsubscribe_status_{
      websocket::SendStatus::kWriteUnavailable};
  bool book_ticker_subscribe_accepted_{false};
  bool trade_subscribe_accepted_{false};
  bool book_ticker_unsubscribe_accepted_{false};
  bool trade_unsubscribe_accepted_{false};
  bool book_ticker_subscribe_sent_{false};
  bool trade_subscribe_sent_{false};
  bool connection_active_{false};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_MARKET_DATA_SUBSCRIPTION_CONTROLLER_H_
