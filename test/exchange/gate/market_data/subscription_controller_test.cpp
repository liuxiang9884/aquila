#include "exchange/gate/market_data/subscription_controller.h"

#include <gtest/gtest.h>

namespace {

TEST(GateSubscriptionControllerTest,
     ActivePhaseRequestsSubscribeUntilSuccessfulSend) {
  aquila::gate::BookTickerSubscriptionController controller;

  EXPECT_TRUE(controller.OnConnectionPhase(
      aquila::websocket::ConnectionPhase::kActive));
  EXPECT_TRUE(controller.connection_active());
  EXPECT_TRUE(controller.ShouldSendBookTickerSubscribe());

  controller.RecordBookTickerSubscribeAttempt(
      aquila::websocket::SendStatus::kNoPreparedWriteSlot);
  EXPECT_EQ(controller.subscribe_status(),
            aquila::websocket::SendStatus::kNoPreparedWriteSlot);
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kIdle);
  EXPECT_TRUE(controller.CanRetrySubscribe());
  EXPECT_TRUE(controller.ShouldSendBookTickerSubscribe());

  controller.RecordBookTickerSubscribeAttempt(
      aquila::websocket::SendStatus::kOk);
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribeSent);
  EXPECT_FALSE(controller.CanRetrySubscribe());
  EXPECT_FALSE(controller.ShouldSendBookTickerSubscribe());
  EXPECT_FALSE(controller.OnConnectionPhase(
      aquila::websocket::ConnectionPhase::kActive));
}

TEST(GateSubscriptionControllerTest,
     MultiFeedRetryDoesNotResendSuccessfulSubscribe) {
  aquila::gate::BookTickerSubscriptionController controller(
      aquila::gate::DataSessionFeeds{.book_ticker = true, .trade = true});

  EXPECT_TRUE(controller.OnConnectionPhase(
      aquila::websocket::ConnectionPhase::kActive));
  EXPECT_TRUE(controller.ShouldSendBookTickerSubscribe());
  EXPECT_TRUE(controller.ShouldSendTradeSubscribe());

  controller.RecordBookTickerSubscribeAttempt(
      aquila::websocket::SendStatus::kOk);
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribeSent);
  EXPECT_FALSE(controller.ShouldSendBookTickerSubscribe());
  EXPECT_TRUE(controller.ShouldSendTradeSubscribe());
  EXPECT_TRUE(controller.CanRetrySubscribe());

  controller.RecordTradeSubscribeAttempt(
      aquila::websocket::SendStatus::kNoPreparedWriteSlot);
  EXPECT_EQ(controller.subscribe_status(),
            aquila::websocket::SendStatus::kNoPreparedWriteSlot);
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribeSent);
  EXPECT_FALSE(controller.ShouldSendBookTickerSubscribe());
  EXPECT_TRUE(controller.ShouldSendTradeSubscribe());
  EXPECT_TRUE(controller.CanRetrySubscribe());

  controller.RecordTradeSubscribeAttempt(aquila::websocket::SendStatus::kOk);
  EXPECT_FALSE(controller.ShouldSendBookTickerSubscribe());
  EXPECT_FALSE(controller.ShouldSendTradeSubscribe());
  EXPECT_FALSE(controller.CanRetrySubscribe());

  controller.MarkBookTickerSubscribeAccepted();
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribeSent);
  controller.MarkTradeSubscribeAccepted();
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribed);
}

TEST(GateSubscriptionControllerTest,
     DisconnectResetsSentOrSubscribedStateButKeepsRejectedSticky) {
  aquila::gate::BookTickerSubscriptionController controller;

  EXPECT_TRUE(controller.OnConnectionPhase(
      aquila::websocket::ConnectionPhase::kActive));
  controller.RecordBookTickerSubscribeAttempt(
      aquila::websocket::SendStatus::kOk);
  controller.MarkBookTickerSubscribeAccepted();
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribed);

  EXPECT_FALSE(controller.OnConnectionPhase(
      aquila::websocket::ConnectionPhase::kReconnectBackoff));
  EXPECT_FALSE(controller.connection_active());
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kIdle);

  EXPECT_TRUE(controller.OnConnectionPhase(
      aquila::websocket::ConnectionPhase::kActive));
  controller.MarkSubscribeRejected();
  EXPECT_FALSE(controller.CanRetrySubscribe());
  EXPECT_FALSE(controller.OnConnectionPhase(
      aquila::websocket::ConnectionPhase::kReconnectBackoff));
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kRejected);
}

TEST(GateSubscriptionControllerTest, TracksUnsubscribeStatusAndAck) {
  aquila::gate::BookTickerSubscriptionController controller;

  controller.RecordUnsubscribeAttempt(aquila::websocket::SendStatus::kOk);
  EXPECT_EQ(controller.unsubscribe_status(),
            aquila::websocket::SendStatus::kOk);
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kUnsubscribeSent);

  controller.MarkBookTickerUnsubscribeAccepted();
  EXPECT_EQ(controller.subscription_state(),
            aquila::gate::SubscriptionState::kUnsubscribed);
}

}  // namespace
