#include "exchange/gate/trading/order_session.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/websocket/message_view.h"
#include "exchange/gate/trading/submit_response_parser.h"

namespace aquila::gate {
namespace {

inline constexpr std::int64_t kReasonableUnixEpochNs =
    1'600'000'000'000'000'000LL;

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

websocket::MessageView PingView(std::string_view payload) noexcept {
  return {
      .kind = websocket::PayloadKind::kPing,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 3,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

struct RecordingHandler {
  std::vector<OrderResponse> responses;
  int login_ready_calls{0};

  void OnOrderResponse(const OrderResponse& response) noexcept {
    responses.push_back(response);
  }

  void OnOrderSessionLoginReady() noexcept {
    ++login_ready_calls;
  }
};

template <typename Handler>
class TestOrderSession
    : public OrderSession<Handler, OrderSessionDefaultPlainWebSocketPolicy,
                          OrderSessionDiagnostics> {
 public:
  explicit TestOrderSession(Handler& handler)
      : OrderSession<Handler, OrderSessionDefaultPlainWebSocketPolicy,
                     OrderSessionDiagnostics>(
            MakeConfig(),
            LoginCredentials{.api_key = "key", .api_secret = "secret"},
            handler) {}

  explicit TestOrderSession(websocket::ConnectionConfig config,
                            Handler& handler)
      : OrderSession<Handler, OrderSessionDefaultPlainWebSocketPolicy,
                     OrderSessionDiagnostics>(
            std::move(config),
            LoginCredentials{.api_key = "key", .api_secret = "secret"},
            handler) {}

  TestOrderSession(Handler& handler, std::size_t request_map_capacity)
      : OrderSession<Handler, OrderSessionDefaultPlainWebSocketPolicy,
                     OrderSessionDiagnostics>(
            MakeConfig(),
            LoginCredentials{.api_key = "key", .api_secret = "secret"}, handler,
            request_map_capacity) {}

  static websocket::ConnectionConfig MakeConfig() {
    websocket::ConnectionConfig config{};
    config.host = "localhost";
    config.service = "80";
    config.target = "/v4/ws/usdt";
    config.prepared_write_slots = 8;
    config.prepared_write_bytes = 4096;
    return config;
  }
};

struct TestOrder {
  std::uint64_t local_order_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  double quantity{0.0};
  std::string_view quantity_text{};
  std::string_view price_text{};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::uint64_t exchange_order_id{0};
  bool reduce_only{false};
};

struct TestOrderWithoutExchangeOrderId {
  std::uint64_t local_order_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  double quantity{0.0};
  std::string_view quantity_text{};
  std::string_view price_text{};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  bool reduce_only{false};
};

TestOrder MakePlaceOrder(std::uint64_t local_order_id) noexcept {
  return TestOrder{.local_order_id = local_order_id,
                   .symbol = "BTC_USDT",
                   .side = OrderSide::kBuy,
                   .type = OrderType::kLimit,
                   .quantity = 1.0,
                   .quantity_text = "1",
                   .price_text = "81000",
                   .time_in_force = TimeInForce::kGoodTillCancel,
                   .exchange_order_id = 0,
                   .reduce_only = false};
}

TestOrder MakeCancelOrder(std::uint64_t local_order_id,
                          std::uint64_t exchange_order_id) noexcept {
  TestOrder order = MakePlaceOrder(local_order_id);
  order.exchange_order_id = exchange_order_id;
  return order;
}

std::string_view LoginSuccessResponse() noexcept {
  return R"({"request_id":"72057594037927937","ack":false,"header":{"status":"200","channel":"futures.login","event":"api"},"data":{"result":{"uid":"1"}}})";
}

std::string_view LoginSuccessResponseWithoutAck() noexcept {
  return R"({"header":{"status":"200","channel":"futures.login","event":"api"},"data":{"result":{"uid":"1"}},"request_id":"72057594037927937"})";
}

std::string_view PlaceAckResponse() noexcept {
  return R"({"request_id":"144115188075855874","ack":true,"header":{"response_time":"1681195484268","status":"200","channel":"futures.order_place","event":"api","x_out_time":1681985856667598},"data":{"result":{"req_id":"144115188075855874"}}})";
}

std::string_view PlaceResultResponse() noexcept {
  return R"({"request_id":"144115188075855874","ack":false,"header":{"response_time":"1681195484360","status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"id":"36028827892199865","text":"t-123"}}})";
}

template <typename Handler>
void ActivateAndLogin(TestOrderSession<Handler>& session) {
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  ASSERT_EQ(session.Handle(TextView(LoginSuccessResponse())),
            websocket::DeliveryResult::kAccepted);
  ASSERT_TRUE(session.login_ready());
}

TEST(OrderSessionTest, SignedSizeUsesSideAndPositiveQuantity) {
  TestOrder buy = MakePlaceOrder(123);
  buy.side = OrderSide::kBuy;
  buy.quantity = 2;
  buy.quantity_text = "2";
  TestOrder sell = buy;
  sell.side = OrderSide::kSell;
  std::array<char, 64> buffer{};

  EXPECT_EQ(SignedOrderSizeTextForGate(buy, buffer), "2");
  EXPECT_EQ(SignedOrderSizeTextForGate(sell, buffer), "-2");
}

TEST(OrderSessionTest, SignedSizeKeepsDecimalQuantityText) {
  TestOrder buy = MakePlaceOrder(123);
  buy.quantity = 0;
  buy.quantity_text = "0.1";
  TestOrder sell = buy;
  sell.side = OrderSide::kSell;
  std::array<char, 64> buffer{};

  EXPECT_EQ(SignedOrderSizeTextForGate(buy, buffer), "0.1");
  EXPECT_EQ(SignedOrderSizeTextForGate(sell, buffer), "-0.1");
}

TEST(OrderSessionTest, RejectsPlaceBeforeLoginReady) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  const OrderSendResult result = session.PlaceOrder(MakePlaceOrder(1));

  EXPECT_EQ(result.status, OrderSendStatus::kNotLoggedIn);
  EXPECT_EQ(result.request_sequence, 0U);
  EXPECT_EQ(result.encoded_request_id, 0U);
  EXPECT_TRUE(handler.responses.empty());
}

TEST(OrderSessionTest, RejectsUnsupportedMarketOrderTypeBeforeSend) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);
  TestOrder order = MakePlaceOrder(123);
  order.type = OrderType::kMarket;

  const OrderSendResult sent = session.PlaceOrder(order);

  EXPECT_EQ(sent.status, OrderSendStatus::kUnsupportedOrderType);
  EXPECT_EQ(sent.request_sequence, 0U);
  EXPECT_EQ(sent.encoded_request_id, 0U);
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.stats().local_send_failures, 1U);
}

TEST(OrderSessionTest, AcceptsGateLoginResponseWithoutAckField) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  ASSERT_EQ(session.Handle(TextView(LoginSuccessResponseWithoutAck())),
            websocket::DeliveryResult::kAccepted);

  EXPECT_TRUE(session.login_ready());
  EXPECT_EQ(handler.login_ready_calls, 1);
  EXPECT_EQ(session.stats().login_accepted, 1U);
  EXPECT_EQ(session.stats().ignored_messages, 0U);
}

TEST(OrderSessionTest, InvalidPlaceLocalOrderIdIsRejectedBeforeSend) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(0));

  EXPECT_EQ(sent.status, OrderSendStatus::kInvalidLocalOrderId);
  EXPECT_NE(sent.request_sequence, 0U);
  EXPECT_NE(sent.encoded_request_id, 0U);
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.stats().local_send_failures, 1U);
}

TEST(OrderSessionTest, SendsLoginOnActiveAndMarksReadyOnSuccess) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  const auto result = session.Handle(TextView(LoginSuccessResponse()));

  EXPECT_EQ(result, websocket::DeliveryResult::kAccepted);
  EXPECT_TRUE(session.login_ready());
  EXPECT_EQ(session.stats().login_sent, 1U);
  EXPECT_EQ(session.stats().login_accepted, 1U);
  EXPECT_EQ(handler.login_ready_calls, 1);
}

TEST(OrderSessionTest, PlaceAckDoesNotEraseCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);
  EXPECT_GT(sent.send_local_ns, kReasonableUnixEpochNs);

  session.Handle(TextView(PlaceAckResponse()));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kAck);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(handler.responses[0].exchange_order_id, 0U);
  EXPECT_EQ(handler.responses[0].request_sequence, 2U);
  EXPECT_GE(handler.responses[0].local_receive_ns, sent.send_local_ns);
  EXPECT_GT(handler.responses[0].local_receive_ns, kReasonableUnixEpochNs);
  EXPECT_EQ(handler.responses[0].exchange_ns, 1681985856667598000LL);
  EXPECT_EQ(session.inflight_count(), 1U);
}

TEST(OrderSessionTest, RequestMapCapacityRejectsAdditionalInflightRequests) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler, 1);
  ActivateAndLogin(session);

  const OrderSendResult first = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(first.status, OrderSendStatus::kOk);
  EXPECT_EQ(session.inflight_count(), 1U);
  EXPECT_EQ(session.request_map_capacity(), 1U);

  const OrderSendResult second = session.PlaceOrder(MakePlaceOrder(124));

  EXPECT_EQ(second.status, OrderSendStatus::kInflightFull);
  EXPECT_EQ(second.request_sequence, 0U);
  EXPECT_EQ(second.encoded_request_id, 0U);
  EXPECT_EQ(session.inflight_count(), 1U);
  EXPECT_EQ(session.stats().local_send_failures, 1U);
}

TEST(OrderSessionTest, PlaceResultMapsExchangeOrderIdAndErasesCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);
  EXPECT_GT(sent.send_local_ns, kReasonableUnixEpochNs);

  session.Handle(TextView(PlaceResultResponse()));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kAccepted);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(handler.responses[0].exchange_order_id, 36028827892199865U);
  EXPECT_GE(handler.responses[0].local_receive_ns, sent.send_local_ns);
  EXPECT_GT(handler.responses[0].local_receive_ns, kReasonableUnixEpochNs);
  EXPECT_EQ(handler.responses[0].exchange_ns, 1681195484360000000LL);
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.exchange_order_id_for_local_order(123), 36028827892199865U);
}

TEST(OrderSessionTest, ExchangeOrderIdCacheStaysWithinRequestMapCapacity) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler, 1);
  ActivateAndLogin(session);

  OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);
  session.Handle(TextView(PlaceResultResponse()));
  EXPECT_EQ(session.exchange_order_id_for_local_order(123), 36028827892199865U);

  sent = session.PlaceOrder(MakePlaceOrder(124));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);
  session.Handle(TextView(
      R"({"request_id":"144115188075855875","ack":false,"header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"id":"36028827892199866","text":"t-124"}}})"));

  ASSERT_EQ(handler.responses.size(), 2U);
  EXPECT_EQ(handler.responses[1].kind, OrderResponseKind::kAccepted);
  EXPECT_EQ(handler.responses[1].local_order_id, 124);
  EXPECT_EQ(handler.responses[1].exchange_order_id, 36028827892199866U);
  EXPECT_EQ(session.exchange_order_id_for_local_order(124), 0U);
}

TEST(OrderSessionTest, ExplicitForgetClearsCachedExchangeOrderId) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);
  session.Handle(TextView(PlaceResultResponse()));

  EXPECT_TRUE(session.forget_exchange_order_id_for_local_order(123));
  EXPECT_EQ(session.exchange_order_id_for_local_order(123), 0U);
  EXPECT_FALSE(session.forget_exchange_order_id_for_local_order(123));
}

TEST(OrderSessionTest, ExplicitFeedbackCacheApiUpdatesAndForgetsExchangeId) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  session.CacheExchangeOrderId(123, 36028827892199865U);
  session.CacheExchangeOrderId(123, 36028827892199866U);

  EXPECT_EQ(session.exchange_order_id_for_local_order(123), 36028827892199866U);

  session.ForgetExchangeOrderId(123);

  EXPECT_EQ(session.exchange_order_id_for_local_order(123), 0U);
}

TEST(OrderSessionTest, CancelUsesExchangeOrderIdPath) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent =
      session.CancelOrder(MakeCancelOrder(123, 36028827892199865U));

  EXPECT_EQ(sent.status, OrderSendStatus::kOk);
  EXPECT_EQ(session.inflight_count(), 1U);
}

TEST(OrderSessionTest, CancelAcceptsOrderWithoutExchangeOrderIdField) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.CancelOrder(
      TestOrderWithoutExchangeOrderId{.local_order_id = 123});

  EXPECT_EQ(sent.status, OrderSendStatus::kOk);
  EXPECT_EQ(session.inflight_count(), 1U);
}

TEST(OrderSessionTest, CancelResultErasesCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent =
      session.CancelOrder(MakeCancelOrder(123, 36028827892199865U));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"216172782113783810","ack":false,"header":{"status":"200","channel":"futures.order_cancel","event":"api"},"data":{"result":{"id":"36028827892199865","text":"t-123"}}})"));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kCancelAccepted);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.exchange_order_id_for_local_order(123), 0U);
}

TEST(OrderSessionTest, CancelResultWithExchangeIdOnlyIsAccepted) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent =
      session.CancelOrder(MakeCancelOrder(123, 36028827892199865U));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"216172782113783810","ack":false,"header":{"status":"200","channel":"futures.order_cancel","event":"api"},"data":{"result":{"id":"36028827892199865"}}})"));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kCancelAccepted);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(handler.responses[0].exchange_order_id, 36028827892199865U);
  EXPECT_EQ(session.inflight_count(), 0U);
}

TEST(OrderSessionTest, CancelResultMissingIdentityErasesWithoutHandler) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent =
      session.CancelOrder(MakeCancelOrder(123, 36028827892199865U));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"216172782113783810","ack":false,"header":{"status":"200","channel":"futures.order_cancel","event":"api"},"data":{"result":{}}})"));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, CancelErrorMapsRejectedAndErasesCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent =
      session.CancelOrder(MakeCancelOrder(123, 36028827892199865U));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"216172782113783810","ack":false,"header":{"status":"400","channel":"futures.order_cancel","event":"api"},"data":{"errs":{"label":"ORDER_NOT_FOUND","message":"order not found"}}})"));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kCancelRejected);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(handler.responses[0].error_label_hash,
            HashGateSubmitString("ORDER_NOT_FOUND"));
  EXPECT_EQ(session.inflight_count(), 0U);
}

TEST(OrderSessionTest, CancelErrorWithoutAckMapsRejectedAndErasesCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent =
      session.CancelOrder(MakeCancelOrder(123, 36028827892199865U));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"216172782113783810","header":{"status":"404","channel":"futures.order_cancel","event":"api"},"data":{"errs":{"label":"ORDER_NOT_FOUND","message":"order not found"}}})"));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kCancelRejected);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(handler.responses[0].http_status, 404);
  EXPECT_EQ(handler.responses[0].error_label_hash,
            HashGateSubmitString("ORDER_NOT_FOUND"));
  EXPECT_EQ(session.inflight_count(), 0U);
}

TEST(OrderSessionTest, DisconnectClearsInflightWithoutFakeResponses) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.CancelOrder(MakeCancelOrder(123, 0));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.OnConnectionPhase(websocket::ConnectionPhase::kDisconnected);

  EXPECT_FALSE(session.login_ready());
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_TRUE(handler.responses.empty());
}

TEST(OrderSessionTest, NonTextMessagesAreAcceptedAndIgnored) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  EXPECT_EQ(session.Handle(BinaryView("abc")),
            websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.Handle(PingView("abc")),
            websocket::DeliveryResult::kAccepted);

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.stats().text_messages, 1U);
}

TEST(OrderSessionTest, UnknownRequestIdIsIgnored) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":false,"header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"id":"36028827892199865","text":"t-123"}}})"));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.stats().unknown_request_ids, 1U);
}

TEST(OrderSessionTest, StaleLoginResponseWithoutSentLoginDoesNotSetReady) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);

  session.Handle(TextView(LoginSuccessResponse()));

  EXPECT_FALSE(session.login_ready());
  EXPECT_EQ(session.stats().login_accepted, 0U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, LoginSendFailureDoesNotAcceptMatchingResponse) {
  RecordingHandler handler;
  websocket::ConnectionConfig config =
      TestOrderSession<RecordingHandler>::MakeConfig();
  config.prepared_write_slots = 0;
  TestOrderSession<RecordingHandler> session(std::move(config), handler);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  session.Handle(TextView(LoginSuccessResponse()));

  EXPECT_FALSE(session.login_ready());
  EXPECT_EQ(session.stats().login_sent, 0U);
  EXPECT_EQ(session.stats().login_accepted, 0U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, DuplicateLoginResponseAfterAcceptedLoginIsIgnored) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  session.Handle(TextView(LoginSuccessResponse()));

  EXPECT_TRUE(session.login_ready());
  EXPECT_EQ(session.stats().login_accepted, 1U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, LoginResponseWithWrongChannelDoesNotSetReady) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  session.Handle(TextView(
      R"({"request_id":"72057594037927937","ack":false,"header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"uid":"1"}}})"));

  EXPECT_FALSE(session.login_ready());
  EXPECT_EQ(session.stats().login_accepted, 0U);
  EXPECT_EQ(session.stats().login_rejected, 0U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);

  session.Handle(TextView(LoginSuccessResponse()));

  EXPECT_FALSE(session.login_ready());
  EXPECT_EQ(session.stats().login_accepted, 0U);
  EXPECT_EQ(session.stats().ignored_messages, 2U);
}

TEST(OrderSessionTest, LoginResponseWithUnknownKindDoesNotSetReady) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  session.Handle(TextView(
      R"({"request_id":"72057594037927937","ack":false,"header":{"status":"200","channel":"futures.login","event":"api"},"data":{}})"));

  EXPECT_FALSE(session.login_ready());
  EXPECT_EQ(session.stats().login_accepted, 0U);
  EXPECT_EQ(session.stats().login_rejected, 0U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);

  session.Handle(TextView(LoginSuccessResponse()));

  EXPECT_FALSE(session.login_ready());
  EXPECT_EQ(session.stats().login_accepted, 0U);
  EXPECT_EQ(session.stats().ignored_messages, 2U);
}

TEST(OrderSessionTest, LoginResultWithoutUidDoesNotSetReady) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  session.Handle(TextView(
      R"({"request_id":"72057594037927937","ack":false,"header":{"status":"200","channel":"futures.login","event":"api"},"data":{"result":{}}})"));

  EXPECT_FALSE(session.login_ready());
  EXPECT_EQ(session.stats().login_accepted, 0U);
  EXPECT_EQ(session.stats().login_rejected, 1U);
}

TEST(OrderSessionTest, AckWithMismatchedReqIdKeepsInflightAndSkipsHandler) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":true,"header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"req_id":"144115188075855875"}}})"));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 1U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, AckWithNonOkStatusKeepsInflightAndSkipsHandler) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":true,"header":{"status":"400","channel":"futures.order_place","event":"api"},"data":{"result":{"req_id":"144115188075855874"}}})"));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 1U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, MissingAckPlaceResultErasesCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"id":"36028827892199865","text":"t-123"}}})"));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kAccepted);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(handler.responses[0].exchange_order_id, 36028827892199865U);
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.stats().ignored_messages, 0U);
}

TEST(OrderSessionTest, MissingAckCancelResultErasesCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent =
      session.CancelOrder(MakeCancelOrder(123, 36028827892199865U));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"216172782113783810","header":{"status":"200","channel":"futures.order_cancel","event":"api"},"data":{"result":{"id":"36028827892199865"}}})"));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kCancelAccepted);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(handler.responses[0].exchange_order_id, 36028827892199865U);
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.stats().ignored_messages, 0U);
}

TEST(OrderSessionTest, NonBoolAckResultKeepsInflightAndSkipsHandler) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":"false","header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"id":"36028827892199865","text":"t-123"}}})"));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 1U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, ResultWithNonOkStatusKeepsInflightAndSkipsHandler) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":false,"header":{"status":"400","channel":"futures.order_place","event":"api"},"data":{"result":{"id":"36028827892199865","text":"t-123"}}})"));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 1U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, ResponseChannelMismatchSkipsHandlerAndKeepsInflight) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":false,"header":{"status":"200","channel":"futures.order_cancel","event":"api"},"data":{"result":{"id":"36028827892199865","text":"t-123"}}})"));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 1U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, UnknownResponseKindSkipsHandlerAndKeepsInflight) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":false,"header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{}})"));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 1U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, PlaceErrorMapsRejectedAndErasesCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":false,"header":{"status":"400","channel":"futures.order_place","event":"api"},"data":{"errs":{"label":"TOO_MANY_REQUESTS","message":"rate limit"}}})"));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kRejected);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(handler.responses[0].error_label_hash,
            HashGateSubmitString("TOO_MANY_REQUESTS"));
  EXPECT_EQ(session.inflight_count(), 0U);
}

TEST(OrderSessionTest, PlaceErrorWithoutAckMapsRejectedAndErasesCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","header":{"status":"400","channel":"futures.order_place","event":"api"},"data":{"errs":{"label":"INVALID_PARAM_VALUE","message":"invalid size"}}})"));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kRejected);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(handler.responses[0].http_status, 400);
  EXPECT_EQ(handler.responses[0].error_label_hash,
            HashGateSubmitString("INVALID_PARAM_VALUE"));
  EXPECT_EQ(session.inflight_count(), 0U);
}

TEST(OrderSessionTest, PlaceResultMissingExchangeIdErasesWithoutHandler) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":false,"header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"text":"t-123"}}})"));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, PlaceResultMismatchedTextErasesWithoutHandler) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":false,"header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"id":"36028827892199865","text":"t-456"}}})"));

  EXPECT_TRUE(handler.responses.empty());
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.stats().ignored_messages, 1U);
}

TEST(OrderSessionTest, NoPreparedWriteSlotsMapsToLocalSendFailure) {
  RecordingHandler handler;
  websocket::ConnectionConfig config =
      TestOrderSession<RecordingHandler>::MakeConfig();
  config.prepared_write_slots = 1;
  TestOrderSession<RecordingHandler> session(std::move(config), handler);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  session.Handle(TextView(LoginSuccessResponse()));
  ASSERT_TRUE(session.login_ready());

  const OrderSendResult sent = session.PlaceOrder(MakePlaceOrder(123));

  EXPECT_EQ(sent.status, OrderSendStatus::kNoPreparedWriteSlot);
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.stats().local_send_failures, 1U);
}

TEST(OrderSessionTest, InvalidCancelFallbackLocalOrderIdFailsInEncoder) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.CancelOrder(MakeCancelOrder(0, 0));

  EXPECT_EQ(sent.status, OrderSendStatus::kInvalidLocalOrderId);
  EXPECT_NE(sent.request_sequence, 0U);
  EXPECT_NE(sent.encoded_request_id, 0U);
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_EQ(session.stats().local_send_failures, 1U);
}

}  // namespace
}  // namespace aquila::gate
