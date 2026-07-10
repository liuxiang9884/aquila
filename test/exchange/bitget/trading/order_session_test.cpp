#include "exchange/bitget/trading/order_session.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/websocket/message_view.h"
#include "core/websocket/types.h"

namespace aquila::bitget {
namespace {

websocket::MessageView TextView(std::string_view payload) {
  return websocket::MessageView{
      .kind = websocket::PayloadKind::kText,
      .payload = std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(payload.data()), payload.size()),
  };
}

struct RecordingHandler {
  std::vector<OrderResponse> responses;
  int ready_calls{0};
  int not_ready_calls{0};

  void OnOrderResponse(const OrderResponse& response) noexcept {
    responses.push_back(response);
  }

  void OnOrderSessionLoginReady() noexcept {
    ++ready_calls;
  }

  void OnOrderSessionLoginNotReady() noexcept {
    ++not_ready_calls;
  }
};

struct TestOrder {
  std::uint64_t local_order_id{0};
  std::uint64_t parent_id{7};
  std::uint64_t exchange_order_id{0};
  std::uint16_t gateway_route_id{3};
  std::string_view symbol{"BTCUSDT"};
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::string_view quantity_text{"0.001"};
  std::string_view price_text{"100000"};
  bool reduce_only{false};
};

template <typename Handler>
class TestOrderSession
    : public OrderSession<Handler, OrderSessionDefaultPlainWebSocketPolicy,
                          OrderSessionDiagnostics> {
 public:
  TestOrderSession(Handler& handler, std::size_t request_capacity = 16,
                   std::size_t cache_capacity = 16,
                   std::size_t prepared_write_slots = 32)
      : OrderSession<Handler, OrderSessionDefaultPlainWebSocketPolicy,
                     OrderSessionDiagnostics>(
            MakeConfig(prepared_write_slots),
            LoginCredentials{.api_key = "key",
                             .api_secret = "secret",
                             .passphrase = "phrase"},
            handler, request_capacity, cache_capacity) {}

 private:
  static websocket::ConnectionConfig MakeConfig(
      std::size_t prepared_write_slots) {
    websocket::ConnectionConfig config{};
    config.host = "localhost";
    config.port = "80";
    config.target = "/v3/ws/private";
    config.enable_tls = false;
    config.prepared_write_slots = prepared_write_slots;
    config.prepared_write_bytes = 4096;
    config.heartbeat_interval_ms = 30000;
    config.heartbeat_timeout_ms = 10000;
    return config;
  }
};

template <typename Handler>
void ActivateAndLogin(TestOrderSession<Handler>* session) {
  session->OnConnectionPhase(websocket::ConnectionPhase::kActive);
  ASSERT_FALSE(session->Ready());
  ASSERT_EQ(
      session->Handle(TextView(R"({"event":"login","code":"0","msg":""})")),
      websocket::DeliveryResult::kAccepted);
  ASSERT_TRUE(session->Ready());
}

std::string SuccessResponse(const OrderSendResult& sent, std::string_view topic,
                            std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) {
  return fmt::format(
      R"({{"event":"trade","id":"{}","topic":"{}","args":[{{"orderId":"{}","clientOid":"a-{}"}}],"code":"0","msg":"success","connId":"connection-1","ts":"1750034397076"}})",
      sent.encoded_request_id, topic, exchange_order_id, local_order_id);
}

std::string ErrorResponse(const OrderSendResult& sent, std::string_view topic,
                          std::uint32_t code) {
  return fmt::format(
      R"({{"event":"error","id":"{}","topic":"{}","code":"{}","msg":"error","ts":"1750034397076"}})",
      sent.encoded_request_id, topic, code);
}

std::string TopiclessErrorResponse(const OrderSendResult& sent,
                                   std::uint32_t code) {
  return fmt::format(
      R"({{"event":"error","id":"{}","code":"{}","msg":"error"}})",
      sent.encoded_request_id, code);
}

TEST(BitgetOrderSessionTest, ActiveSendsLoginAndSuccessMarksReady) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  EXPECT_TRUE(session.active());
  EXPECT_FALSE(session.Ready());
  EXPECT_EQ(session.stats().login_sent, 1U);

  session.Handle(TextView(R"({"event":"login","code":"0","msg":""})"));

  EXPECT_TRUE(session.Ready());
  EXPECT_EQ(handler.ready_calls, 1);
  EXPECT_EQ(session.stats().login_accepted, 1U);
}

TEST(BitgetOrderSessionTest, DegradedRecoveryDoesNotSendAnotherLogin) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);

  session.OnConnectionPhase(websocket::ConnectionPhase::kDegraded);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  EXPECT_TRUE(session.Ready());
  EXPECT_EQ(session.stats().login_sent, 1U);
  EXPECT_EQ(handler.ready_calls, 1);
}

TEST(BitgetOrderSessionTest, LoginFailureAndServiceUpgradeStayNotReady) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  session.Handle(TextView(R"({"event":"error","code":"30005","msg":"error"})"));
  EXPECT_FALSE(session.Ready());
  EXPECT_EQ(session.stats().login_rejected, 1U);

  session.OnConnectionPhase(websocket::ConnectionPhase::kReconnectBackoff);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  session.Handle(TextView(R"({"event":"login","code":"0","msg":""})"));
  ASSERT_TRUE(session.Ready());
  session.Handle(
      TextView(R"({"event":"error","code":"30033","msg":"upgrade"})"));
  EXPECT_FALSE(session.Ready());
  EXPECT_TRUE(session.reconnect_requested_for_test());
}

TEST(BitgetOrderSessionTest, ConnectionLimitErrorAfterLoginRequestsReconnect) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);

  session.Handle(TextView(R"({"event":"error","code":"30007","msg":"limit"})"));

  EXPECT_FALSE(session.Ready());
  EXPECT_TRUE(session.reconnect_requested_for_test());
}

TEST(BitgetOrderSessionTest,
     OperationAuthenticationErrorsInvalidateReadyAndReconnect) {
  for (const std::uint32_t code : {30004U, 30007U, 30033U}) {
    RecordingHandler handler;
    TestOrderSession<RecordingHandler> session(handler);
    ActivateAndLogin(&session);
    const OrderSendResult sent =
        session.PlaceOrder(TestOrder{.local_order_id = code});
    ASSERT_EQ(sent.status, OrderSendStatus::kOk) << code;

    session.Handle(TextView(ErrorResponse(sent, "place-order", code)));

    ASSERT_EQ(handler.responses.size(), 1U) << code;
    EXPECT_FALSE(session.Ready()) << code;
    EXPECT_TRUE(session.reconnect_requested_for_test()) << code;
    EXPECT_EQ(handler.not_ready_calls, 1) << code;
  }
}

TEST(BitgetOrderSessionTest,
     TopiclessConnectionErrorInvalidatesWithoutConsumingCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);
  const OrderSendResult sent =
      session.PlaceOrder(TestOrder{.local_order_id = 123});
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(TopiclessErrorResponse(sent, 30007)));

  EXPECT_FALSE(session.Ready());
  EXPECT_TRUE(session.reconnect_requested_for_test());
  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 1U);
}

TEST(BitgetOrderSessionTest, LoginWriteFailureRequestsReconnect) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler, /*request_capacity=*/16,
                                             /*cache_capacity=*/16,
                                             /*prepared_write_slots=*/0);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  EXPECT_FALSE(session.Ready());
  EXPECT_EQ(session.stats().login_sent, 0U);
  EXPECT_TRUE(session.reconnect_requested_for_test());
}

TEST(BitgetOrderSessionTest, PlaceAckCorrelatesCachesAndDoesNotAccept) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);
  const TestOrder order{.local_order_id = 123};

  const OrderSendResult sent = session.PlaceOrder(order);
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);
  EXPECT_EQ(session.inflight_count(), 1U);

  session.Handle(TextView(SuccessResponse(sent, "place-order", 123, 9988)));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kAck);
  EXPECT_EQ(handler.responses[0].local_order_id, 123U);
  EXPECT_EQ(handler.responses[0].parent_id, 7U);
  EXPECT_EQ(handler.responses[0].route_id, 3U);
  EXPECT_EQ(handler.responses[0].exchange_order_id, 9988U);
  EXPECT_EQ(handler.responses[0].exchange_ns, 1750034397076000000LL);
  EXPECT_EQ(handler.responses[0].request_send_local_ns, sent.send_local_ns);
  EXPECT_GE(handler.responses[0].ack_rtt_ns, 0);
  EXPECT_NE(handler.responses[0].connection_id_hash, 0U);
  EXPECT_EQ(session.exchange_order_id_for_local_order(123), 9988U);
  EXPECT_EQ(session.inflight_count(), 0U);
}

TEST(BitgetOrderSessionTest, CancelAckIsGenericAndKeepsCache) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);
  session.CacheExchangeOrderId(123, 9988);
  const TestOrder order{.local_order_id = 123};

  const OrderSendResult sent = session.CancelOrder(order);
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);
  session.Handle(TextView(SuccessResponse(sent, "cancel-order", 123, 9988)));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kAck);
  EXPECT_EQ(handler.responses[0].request_type, OrderRequestType::kCancelOrder);
  EXPECT_EQ(session.exchange_order_id_for_local_order(123), 9988U);
  EXPECT_EQ(session.inflight_count(), 0U);
}

TEST(BitgetOrderSessionTest, CancelAckRejectsMismatchedExchangeOrderId) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);
  session.CacheExchangeOrderId(123, 9988);
  const OrderSendResult sent =
      session.CancelOrder(TestOrder{.local_order_id = 123});
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(SuccessResponse(sent, "cancel-order", 123, 9999)));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 1U);
  EXPECT_EQ(session.stats().correlation_mismatches, 1U);
}

TEST(BitgetOrderSessionTest, MapsDefiniteAndAmbiguousErrors) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);

  const OrderSendResult rejected =
      session.PlaceOrder(TestOrder{.local_order_id = 123});
  ASSERT_EQ(rejected.status, OrderSendStatus::kOk);
  session.Handle(TextView(ErrorResponse(rejected, "place-order", 25202)));

  const OrderSendResult unknown =
      session.CancelOrder(TestOrder{.local_order_id = 123});
  ASSERT_EQ(unknown.status, OrderSendStatus::kOk);
  session.Handle(TextView(ErrorResponse(unknown, "cancel-order", 40010)));

  ASSERT_EQ(handler.responses.size(), 2U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kRejected);
  EXPECT_EQ(handler.responses[1].kind, OrderResponseKind::kUnknownResult);
  EXPECT_EQ(session.inflight_count(), 0U);
}

TEST(BitgetOrderSessionTest, MismatchedClientOidDoesNotConsumeCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);
  const OrderSendResult sent =
      session.PlaceOrder(TestOrder{.local_order_id = 123});
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(SuccessResponse(sent, "place-order", 124, 9988)));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 1U);
  EXPECT_EQ(session.stats().correlation_mismatches, 1U);
}

TEST(BitgetOrderSessionTest, UnknownRequestIdIsIgnored) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);
  const OrderSendResult unknown{
      .status = OrderSendStatus::kOk,
      .encoded_request_id =
          RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, 99),
  };

  session.Handle(TextView(SuccessResponse(unknown, "place-order", 123, 9988)));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.stats().unknown_request_ids, 1U);
}

TEST(BitgetOrderSessionTest, EnforcesInflightAndCacheCapacity) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> inflight_session(handler, 1, 2);
  ActivateAndLogin(&inflight_session);
  ASSERT_EQ(inflight_session.PlaceOrder(TestOrder{.local_order_id = 1}).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(inflight_session.PlaceOrder(TestOrder{.local_order_id = 2}).status,
            OrderSendStatus::kInflightFull);

  RecordingHandler cache_handler;
  TestOrderSession<RecordingHandler> cache_session(cache_handler, 4, 1);
  ActivateAndLogin(&cache_session);
  const OrderSendResult first =
      cache_session.PlaceOrder(TestOrder{.local_order_id = 1});
  ASSERT_EQ(first.status, OrderSendStatus::kOk);
  cache_session.Handle(
      TextView(SuccessResponse(first, "place-order", 1, 9988)));
  EXPECT_EQ(cache_session.PlaceOrder(TestOrder{.local_order_id = 2}).status,
            OrderSendStatus::kOrderIdCacheFull);
}

TEST(BitgetOrderSessionTest, DisconnectClearsStateWithoutSyntheticResponse) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);
  const OrderSendResult sent =
      session.PlaceOrder(TestOrder{.local_order_id = 123});
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);
  session.CacheExchangeOrderId(9, 99);

  session.OnConnectionPhase(websocket::ConnectionPhase::kDisconnected);

  EXPECT_FALSE(session.Ready());
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.exchange_order_id_for_local_order(9), 0U);
  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(handler.not_ready_calls, 1);
}

TEST(BitgetOrderSessionTest, ApplicationPingPongAndTimeoutRequestsReconnect) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(&session);
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
