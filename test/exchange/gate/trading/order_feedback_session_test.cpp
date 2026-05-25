#include "exchange/gate/trading/order_feedback_session.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

#include "core/trading/order_feedback_event.h"
#include "core/websocket/message_view.h"
#include "evaluation/exchange/gate/trading/order_feedback_payload_builder.h"

namespace aquila::gate {
namespace {

using evaluation::BuildOrderFeedbackOrdersPayload;

constexpr std::uint64_t kLocalOrderId =
    evaluation::kOrderFeedbackPayloadLocalOrderId;
constexpr std::uint64_t kExchangeOrderId =
    evaluation::kOrderFeedbackPayloadExchangeOrderId;
constexpr std::int64_t kLocalReceiveNs = 1'770'000'000'001'222'000;

websocket::MessageView TextView(std::string_view payload) noexcept {
  return {
      .kind = websocket::PayloadKind::kText,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 1,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

websocket::MessageView BinaryView(std::string_view payload) noexcept {
  return {
      .kind = websocket::PayloadKind::kBinary,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 2,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

struct RecordingPublisher {
  bool publish_result{true};
  bool global_continuity_lost_result{true};
  int publish_calls{0};
  int global_continuity_lost_calls{0};
  OrderFeedbackEvent last_event{};
  OrderFeedbackContinuityReason last_continuity_reason{
      OrderFeedbackContinuityReason::kUnknown};
  std::int64_t last_continuity_lost_receive_ns{0};

  bool Publish(const OrderFeedbackEvent& event) noexcept {
    ++publish_calls;
    last_event = event;
    return publish_result;
  }

  bool PublishGlobalContinuityLost(OrderFeedbackContinuityReason reason,
                                   std::int64_t local_receive_ns) noexcept {
    ++global_continuity_lost_calls;
    last_continuity_reason = reason;
    last_continuity_lost_receive_ns = local_receive_ns;
    return global_continuity_lost_result;
  }
};

using Session =
    OrderFeedbackSession<RecordingPublisher,
                         OrderFeedbackSessionDefaultPlainWebSocketPolicy,
                         OrderFeedbackSessionDiagnostics>;

websocket::ConnectionConfig MakeConfig() {
  websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "80";
  config.target = "/v4/ws/usdt";
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 4096;
  return config;
}

Session MakeSession(RecordingPublisher& publisher) {
  return Session(MakeConfig(),
                 LoginCredentials{.api_key = "key", .api_secret = "secret"},
                 publisher);
}

std::string_view LoginSuccessResponse() noexcept {
  return R"({"request_id":"72057594037927937","ack":false,"header":{"status":"200","channel":"futures.login","event":"api"},"data":{"result":{"uid":"14391412"}}})";
}

std::string_view SubscribeSuccessResponse() noexcept {
  return R"({"time":1,"channel":"futures.orders","event":"subscribe","result":{"status":"success"}})";
}

}  // namespace

TEST(OrderFeedbackSessionTest, ActiveConnectionSendsLogin) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  EXPECT_FALSE(session.login_ready());
  EXPECT_NE(session.last_login_request().find(R"("channel":"futures.login")"),
            std::string_view::npos);
  EXPECT_EQ(session.stats().login_sent, 1U);
}

TEST(OrderFeedbackSessionTest, LoginSuccessSendsOrdersSubscribe) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  ASSERT_EQ(session.Handle(TextView(LoginSuccessResponse())),
            websocket::DeliveryResult::kAccepted);

  EXPECT_TRUE(session.login_ready());
  EXPECT_EQ(session.stats().login_accepted, 1U);
  EXPECT_NE(
      session.last_subscribe_request().find(R"("channel":"futures.orders")"),
      std::string_view::npos);
  EXPECT_NE(session.last_subscribe_request().find(R"("event":"subscribe")"),
            std::string_view::npos);
  EXPECT_NE(session.last_subscribe_request().find(R"("14391412")"),
            std::string_view::npos);
  EXPECT_NE(session.last_subscribe_request().find(R"("!all")"),
            std::string_view::npos);
  EXPECT_NE(session.last_subscribe_request().find(R"("SIGN":")"),
            std::string_view::npos);
  EXPECT_EQ(session.stats().subscribe_sent, 1U);
}

TEST(OrderFeedbackSessionTest, SubscribeAckMarksReady) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  ASSERT_EQ(session.Handle(TextView(LoginSuccessResponse())),
            websocket::DeliveryResult::kAccepted);

  ASSERT_EQ(session.Handle(TextView(SubscribeSuccessResponse())),
            websocket::DeliveryResult::kAccepted);

  EXPECT_TRUE(session.ready());
  EXPECT_EQ(session.stats().subscribe_acks, 1U);
}

TEST(OrderFeedbackSessionTest, StaleSubscribeAckWithoutSentSubscribeIsIgnored) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);

  ASSERT_EQ(session.Handle(TextView(SubscribeSuccessResponse())),
            websocket::DeliveryResult::kAccepted);

  EXPECT_FALSE(session.ready());
  EXPECT_EQ(session.stats().subscribe_acks, 0U);
  EXPECT_EQ(session.stats().ignored_text_messages, 1U);
}

TEST(OrderFeedbackSessionTest, BinaryOrdersPayloadPublishesEvent) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  std::array<char, 512> buffer{};
  const std::string_view payload = BuildOrderFeedbackOrdersPayload(&buffer);

  ASSERT_EQ(session.Handle(BinaryView(payload)),
            websocket::DeliveryResult::kAccepted);

  EXPECT_EQ(publisher.publish_calls, 1);
  EXPECT_EQ(publisher.last_event.kind, OrderFeedbackKind::kAccepted);
  EXPECT_EQ(publisher.last_event.local_order_id, kLocalOrderId);
  EXPECT_EQ(publisher.last_event.exchange_order_id, kExchangeOrderId);
  EXPECT_EQ(session.stats().binary_messages, 1U);
  EXPECT_EQ(session.stats().events_published, 1U);
}

TEST(OrderFeedbackSessionTest, MalformedBinaryPayloadIncrementsDiagnostics) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);

  ASSERT_EQ(session.Handle(BinaryView("x")),
            websocket::DeliveryResult::kAccepted);

  EXPECT_EQ(publisher.publish_calls, 0);
  EXPECT_EQ(session.stats().binary_messages, 1U);
  EXPECT_EQ(session.stats().parse_errors, 1U);
}

TEST(OrderFeedbackSessionTest,
     DisconnectAfterActivePublishesGlobalContinuityLost) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  session.OnConnectionPhase(websocket::ConnectionPhase::kDisconnected);

  EXPECT_EQ(publisher.global_continuity_lost_calls, 1);
  EXPECT_EQ(publisher.last_continuity_reason,
            OrderFeedbackContinuityReason::kSessionDisconnected);
  EXPECT_EQ(session.stats().global_continuity_lost_events_published, 1U);
  EXPECT_EQ(session.stats().last_continuity_lost_phase,
            websocket::ConnectionPhase::kDisconnected);
  EXPECT_EQ(session.stats().last_continuity_lost_error,
            websocket::ConnectionError::kNone);
}

TEST(OrderFeedbackSessionTest, PublishFailureIncrementsDiagnostics) {
  RecordingPublisher publisher;
  publisher.publish_result = false;
  Session session = MakeSession(publisher);
  std::array<char, 512> buffer{};
  const std::string_view payload = BuildOrderFeedbackOrdersPayload(&buffer);

  ASSERT_EQ(session.Handle(BinaryView(payload)),
            websocket::DeliveryResult::kAccepted);

  EXPECT_EQ(publisher.publish_calls, 1);
  EXPECT_EQ(session.stats().events_published, 0U);
  EXPECT_EQ(session.stats().publish_failures, 1U);
}

}  // namespace aquila::gate
