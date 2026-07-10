#include "exchange/bitget/trading/order_feedback_session.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/trading/order_feedback_event.h"
#include "core/websocket/message_view.h"
#include "core/websocket/types.h"

namespace aquila::bitget {
namespace {

websocket::MessageView TextView(std::string_view payload) noexcept {
  return {
      .kind = websocket::PayloadKind::kText,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .readable_tail_bytes = 0,
  };
}

struct RecordingPublisher {
  bool publish_result{true};
  bool global_result{true};
  std::vector<OrderFeedbackEvent> events;
  std::vector<OrderFeedbackContinuityReason> continuity_reasons;

  bool Publish(const OrderFeedbackEvent& event) noexcept {
    events.push_back(event);
    return publish_result;
  }

  bool PublishGlobalContinuityLost(OrderFeedbackContinuityReason reason,
                                   std::int64_t) noexcept {
    continuity_reasons.push_back(reason);
    return global_result;
  }
};

using Session =
    OrderFeedbackSession<RecordingPublisher,
                         OrderFeedbackSessionDefaultPlainWebSocketPolicy,
                         OrderFeedbackSessionDiagnostics>;

websocket::ConnectionConfig MakeConfig() {
  websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.port = "80";
  config.target = "/v3/ws/private";
  config.enable_tls = false;
  config.prepared_write_slots = 16;
  config.prepared_write_bytes = 4096;
  config.heartbeat_interval_ms = 30000;
  config.heartbeat_timeout_ms = 10000;
  return config;
}

Session MakeSession(RecordingPublisher& publisher) {
  return Session(
      MakeConfig(),
      LoginCredentials{
          .api_key = "key", .api_secret = "secret", .passphrase = "phrase"},
      publisher);
}

constexpr std::string_view kLoginSuccess =
    R"({"event":"login","code":"0","msg":""})";
constexpr std::string_view kSubscribeSuccess =
    R"({"event":"subscribe","arg":{"instType":"UTA","topic":"order"}})";
constexpr std::string_view kAcceptedOrder = R"({
  "action":"snapshot","arg":{"instType":"UTA","topic":"order"},
  "data":[{"category":"usdt-futures","orderId":"9988",
    "clientOid":"a-72057594037927978","qty":"1.5",
    "holdMode":"one_way_mode","marginMode":"crossed",
    "cumExecQty":"0","avgPrice":"0","orderStatus":"new",
    "updatedTime":"1750034397076"}]})";
constexpr std::string_view kMalformedAquilaOrder = R"({
  "action":"snapshot","arg":{"instType":"UTA","topic":"order"},
  "data":[{"clientOid":"a-42"}]})";
constexpr std::string_view kForeignOrder = R"({
  "action":"snapshot","arg":{"instType":"UTA","topic":"order"},
  "data":[{"clientOid":"manual-42"}]})";

void ActivateAndSubscribe(Session* session) {
  session->OnConnectionPhase(websocket::ConnectionPhase::kActive);
  ASSERT_EQ(session->Handle(TextView(kLoginSuccess)),
            websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(session->Handle(TextView(kSubscribeSuccess)),
            websocket::DeliveryResult::kAccepted);
  ASSERT_TRUE(session->Ready());
}

TEST(BitgetOrderFeedbackSessionTest, LoginSubscribeAckMarksReady) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  EXPECT_TRUE(session.active());
  EXPECT_FALSE(session.Ready());
  EXPECT_EQ(session.stats().login_sent, 1U);
  EXPECT_NE(session.last_login_request().find(R"("op":"login")"),
            std::string_view::npos);

  session.Handle(TextView(kLoginSuccess));

  EXPECT_TRUE(session.login_ready());
  EXPECT_FALSE(session.Ready());
  EXPECT_EQ(session.stats().login_accepted, 1U);
  EXPECT_EQ(session.stats().subscribe_sent, 1U);
  EXPECT_EQ(
      session.last_subscribe_request(),
      R"({"op":"subscribe","args":[{"instType":"UTA","topic":"order"}]})");

  session.Handle(TextView(kSubscribeSuccess));

  EXPECT_TRUE(session.subscribed());
  EXPECT_TRUE(session.Ready());
  EXPECT_EQ(session.stats().subscribe_acks, 1U);
}

TEST(BitgetOrderFeedbackSessionTest, StaleAcksAndDegradedRecoveryDoNotAdvance) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);

  session.Handle(TextView(kLoginSuccess));
  session.Handle(TextView(kSubscribeSuccess));
  EXPECT_FALSE(session.Ready());
  EXPECT_EQ(session.stats().ignored_messages, 2U);

  ActivateAndSubscribe(&session);
  session.OnConnectionPhase(websocket::ConnectionPhase::kDegraded);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  EXPECT_TRUE(session.Ready());
  EXPECT_EQ(session.stats().login_sent, 1U);
  EXPECT_EQ(session.stats().subscribe_sent, 1U);
}

TEST(BitgetOrderFeedbackSessionTest, LoginAndSubscribeErrorsStayNotReady) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  session.Handle(TextView(R"({"event":"error","code":"30005","msg":"error"})"));
  EXPECT_FALSE(session.Ready());
  EXPECT_EQ(session.stats().login_rejected, 1U);

  session.OnConnectionPhase(websocket::ConnectionPhase::kReconnectBackoff);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  session.Handle(TextView(kLoginSuccess));
  ASSERT_TRUE(session.login_ready());
  session.Handle(TextView(
      R"({"event":"error","arg":{"instType":"UTA","topic":"order"},"code":"30001","msg":"subscribe failed"})"));

  EXPECT_FALSE(session.Ready());
  EXPECT_EQ(session.stats().subscribe_errors, 1U);
}

TEST(BitgetOrderFeedbackSessionTest, ServiceUpgradeRequestsReconnect) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  ActivateAndSubscribe(&session);

  session.Handle(
      TextView(R"({"event":"error","code":"30033","msg":"upgrade"})"));

  EXPECT_FALSE(session.Ready());
  EXPECT_TRUE(session.reconnect_requested_for_test());
}

TEST(BitgetOrderFeedbackSessionTest, PublishesOrderAndRecordsPublishFailure) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);

  EXPECT_EQ(session.Handle(TextView(kAcceptedOrder)),
            websocket::DeliveryResult::kAccepted);

  ASSERT_EQ(publisher.events.size(), 1U);
  EXPECT_EQ(publisher.events[0].kind, OrderFeedbackKind::kAccepted);
  EXPECT_EQ(session.stats().events_published, 1U);

  publisher.publish_result = false;
  EXPECT_EQ(session.Handle(TextView(kAcceptedOrder)),
            websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(publisher.events.size(), 2U);
  EXPECT_EQ(session.stats().publish_failures, 1U);
}

TEST(BitgetOrderFeedbackSessionTest,
     DecodeContinuityLossIsLatchedPerConnectionGeneration) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  session.Handle(TextView(kMalformedAquilaOrder));
  session.Handle(TextView(kMalformedAquilaOrder));
  session.Handle(TextView(kForeignOrder));

  ASSERT_EQ(publisher.continuity_reasons.size(), 1U);
  EXPECT_EQ(publisher.continuity_reasons[0],
            OrderFeedbackContinuityReason::kDecodeUnrecoverable);
  EXPECT_EQ(session.stats().decode_continuity_lost_events, 1U);
  EXPECT_EQ(session.parser_stats().foreign_orders_ignored, 1U);

  session.OnConnectionPhase(websocket::ConnectionPhase::kDisconnected);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  session.Handle(TextView(kMalformedAquilaOrder));

  ASSERT_EQ(publisher.continuity_reasons.size(), 3U);
  EXPECT_EQ(publisher.continuity_reasons[1],
            OrderFeedbackContinuityReason::kSessionDisconnected);
  EXPECT_EQ(publisher.continuity_reasons[2],
            OrderFeedbackContinuityReason::kDecodeUnrecoverable);
  EXPECT_EQ(session.stats().decode_continuity_lost_events, 2U);
}

TEST(BitgetOrderFeedbackSessionTest, DisconnectAfterActiveBroadcastsOnce) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  session.OnConnectionPhase(websocket::ConnectionPhase::kDisconnected);
  session.OnConnectionPhase(websocket::ConnectionPhase::kClosed);

  ASSERT_EQ(publisher.continuity_reasons.size(), 1U);
  EXPECT_EQ(publisher.continuity_reasons[0],
            OrderFeedbackContinuityReason::kSessionDisconnected);
  EXPECT_EQ(session.stats().disconnect_continuity_lost_events, 1U);
  EXPECT_FALSE(session.Ready());
}

TEST(BitgetOrderFeedbackSessionTest,
     ApplicationPingPongAndTimeoutRequestReconnect) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  ActivateAndSubscribe(&session);
  const std::uint64_t initial = session.application_last_ping_ns_for_test();

  session.AdvanceApplicationHeartbeatForTest(initial + 30'000'000'000ULL);

  EXPECT_TRUE(session.application_awaiting_pong_for_test());
  EXPECT_EQ(session.stats().pings_sent, 1U);
  session.Handle(TextView("pong"));
  EXPECT_FALSE(session.application_awaiting_pong_for_test());
  EXPECT_EQ(session.stats().pongs_received, 1U);

  session.AdvanceApplicationHeartbeatForTest(initial + 60'000'000'000ULL);
  ASSERT_TRUE(session.application_awaiting_pong_for_test());
  session.AdvanceApplicationHeartbeatForTest(initial + 70'000'000'000ULL);

  EXPECT_FALSE(session.Ready());
  EXPECT_TRUE(session.reconnect_requested_for_test());
  EXPECT_EQ(session.stats().heartbeat_timeouts, 1U);
}

}  // namespace
}  // namespace aquila::bitget
