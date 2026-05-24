#include "exchange/gate/trading/order_session_runtime_adapter.h"

#include <array>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/trading/order_manager.h"
#include "core/trading/order_types.h"
#include "core/websocket/types.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::gate {
namespace {

struct FakeRuntime {
  std::vector<core::OrderResponseEvent> responses;
  std::thread::id callback_thread;

  void OnOrderResponse(const core::OrderResponseEvent& event) noexcept {
    callback_thread = std::this_thread::get_id();
    responses.push_back(event);
  }
};

std::array<detail::OrderSessionRuntimeErrorResponseLogRecordForTest, 4>
    g_logged_errors{};
std::array<std::size_t, 4> g_response_count_at_log{};
std::size_t g_logged_error_count{0};
const FakeRuntime* g_runtime_seen_by_log{nullptr};

void CaptureOrderSessionRuntimeErrorResponseLogForTest(
    const detail::OrderSessionRuntimeErrorResponseLogRecordForTest&
        record) noexcept {
  if (g_logged_error_count >= g_logged_errors.size()) {
    return;
  }
  g_logged_errors[g_logged_error_count] = record;
  g_response_count_at_log[g_logged_error_count] =
      g_runtime_seen_by_log == nullptr
          ? 0
          : g_runtime_seen_by_log->responses.size();
  ++g_logged_error_count;
}

void ResetOrderSessionRuntimeErrorResponseLogCapture() noexcept {
  g_logged_errors = {};
  g_response_count_at_log = {};
  g_logged_error_count = 0;
  g_runtime_seen_by_log = nullptr;
  detail::SetOrderSessionRuntimeErrorResponseLogObserverForTest(nullptr);
}

class OrderSessionRuntimeErrorResponseLogCaptureGuard {
 public:
  explicit OrderSessionRuntimeErrorResponseLogCaptureGuard(
      const FakeRuntime& runtime) noexcept {
    ResetOrderSessionRuntimeErrorResponseLogCapture();
    g_runtime_seen_by_log = &runtime;
    detail::SetOrderSessionRuntimeErrorResponseLogObserverForTest(
        CaptureOrderSessionRuntimeErrorResponseLogForTest);
  }

  ~OrderSessionRuntimeErrorResponseLogCaptureGuard() noexcept {
    ResetOrderSessionRuntimeErrorResponseLogCapture();
  }

  OrderSessionRuntimeErrorResponseLogCaptureGuard(
      const OrderSessionRuntimeErrorResponseLogCaptureGuard&) = delete;
  OrderSessionRuntimeErrorResponseLogCaptureGuard& operator=(
      const OrderSessionRuntimeErrorResponseLogCaptureGuard&) = delete;
};

websocket::ConnectionConfig MakeConnectionConfig() {
  websocket::ConnectionConfig config;
  config.host = "127.0.0.1";
  config.target = "/v4/ws/usdt";
  config.service = "1";
  config.enable_tls = false;
  return config;
}

gate::LoginCredentials MakeCredentials() {
  return gate::LoginCredentials{.api_key = "test_key",
                                .api_secret = "test_secret"};
}

TEST(OrderSessionRuntimeAdapterTest,
     LogsGateErrorResponsesBeforeRuntimeDispatch) {
  OrderSessionRuntimeAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>
      adapter(MakeConnectionConfig(), MakeCredentials());
  FakeRuntime runtime;
  OrderSessionRuntimeErrorResponseLogCaptureGuard log_capture(runtime);

  adapter.BindRuntime(runtime);
  adapter.PushOrderResponseForTest(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kAccepted,
      .local_order_id = 11,
      .exchange_order_id = 111,
      .request_sequence = 10,
      .http_status = 200,
      .error_label_hash = 1,
  });
  adapter.PushOrderResponseForTest(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kRejected,
      .local_order_id = 12,
      .exchange_order_id = 0,
      .request_sequence = 20,
      .http_status = 400,
      .error_label_hash = 222,
      .local_receive_ns = 1'770'000'000'000'001'234LL,
      .exchange_ns = 1'770'000'000'000'001'000LL,
  });
  adapter.PushOrderResponseForTest(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kCancelRejected,
      .local_order_id = 13,
      .exchange_order_id = 333,
      .request_sequence = 30,
      .http_status = 404,
      .error_label_hash = 444,
      .local_receive_ns = 1'770'000'000'000'002'345LL,
      .exchange_ns = 1'770'000'000'000'002'000LL,
  });
  adapter.PushOrderResponseForTest(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kCancelAccepted,
      .local_order_id = 14,
      .exchange_order_id = 555,
      .request_sequence = 40,
      .http_status = 200,
      .error_label_hash = 666,
  });

  ASSERT_EQ(runtime.responses.size(), 4U);
  ASSERT_EQ(g_logged_error_count, 2U);
  EXPECT_EQ(g_logged_errors[0].kind, gate::OrderResponseKind::kRejected);
  EXPECT_EQ(g_logged_errors[0].local_order_id, 12U);
  EXPECT_EQ(g_logged_errors[0].exchange_order_id, 0U);
  EXPECT_EQ(g_logged_errors[0].request_sequence, 20U);
  EXPECT_EQ(g_logged_errors[0].http_status, 400);
  EXPECT_EQ(g_logged_errors[0].error_label_hash, 222U);
  EXPECT_EQ(g_logged_errors[0].local_receive_ns, 1'770'000'000'000'001'234LL);
  EXPECT_EQ(g_logged_errors[0].exchange_ns, 1'770'000'000'000'001'000LL);
  EXPECT_EQ(g_logged_errors[0].exchange_to_local_ns, 234);
  EXPECT_EQ(g_response_count_at_log[0], 1U);
  EXPECT_EQ(g_logged_errors[1].kind, gate::OrderResponseKind::kCancelRejected);
  EXPECT_EQ(g_logged_errors[1].local_order_id, 13U);
  EXPECT_EQ(g_logged_errors[1].exchange_order_id, 333U);
  EXPECT_EQ(g_logged_errors[1].request_sequence, 30U);
  EXPECT_EQ(g_logged_errors[1].http_status, 404);
  EXPECT_EQ(g_logged_errors[1].error_label_hash, 444U);
  EXPECT_EQ(g_logged_errors[1].local_receive_ns, 1'770'000'000'000'002'345LL);
  EXPECT_EQ(g_logged_errors[1].exchange_ns, 1'770'000'000'000'002'000LL);
  EXPECT_EQ(g_logged_errors[1].exchange_to_local_ns, 345);
  EXPECT_EQ(g_response_count_at_log[1], 2U);
}

TEST(OrderSessionRuntimeAdapterTest, ConvertsGateResponsesToCoreEvents) {
  const gate::OrderResponse accepted{
      .kind = gate::OrderResponseKind::kAccepted,
      .local_order_id = 0x0400000000000007ULL,
      .exchange_order_id = 36028827892199865ULL,
      .request_sequence = 42,
      .http_status = 200,
      .error_label_hash = 99,
      .local_receive_ns = 123456789,
      .exchange_ns = 1681985856667598000LL,
  };

  const core::OrderResponseEvent event = ToCoreOrderResponseEvent(accepted);

  EXPECT_EQ(event.kind, core::OrderResponseKind::kAccepted);
  EXPECT_EQ(event.local_order_id, accepted.local_order_id);
  EXPECT_EQ(event.exchange_order_id, accepted.exchange_order_id);
  EXPECT_EQ(event.local_receive_ns, accepted.local_receive_ns);
  EXPECT_EQ(event.exchange_ns, accepted.exchange_ns);
}

TEST(OrderSessionRuntimeAdapterTest, ConvertsEveryGateResponseKind) {
  EXPECT_EQ(ToCoreOrderResponseKind(gate::OrderResponseKind::kAck),
            core::OrderResponseKind::kAck);
  EXPECT_EQ(ToCoreOrderResponseKind(gate::OrderResponseKind::kAccepted),
            core::OrderResponseKind::kAccepted);
  EXPECT_EQ(ToCoreOrderResponseKind(gate::OrderResponseKind::kRejected),
            core::OrderResponseKind::kRejected);
  EXPECT_EQ(ToCoreOrderResponseKind(gate::OrderResponseKind::kCancelAccepted),
            core::OrderResponseKind::kCancelAccepted);
  EXPECT_EQ(ToCoreOrderResponseKind(gate::OrderResponseKind::kCancelRejected),
            core::OrderResponseKind::kCancelRejected);
}

TEST(OrderSessionRuntimeAdapterTest,
     BindRuntimeDispatchesOrderResponsesSynchronously) {
  OrderSessionRuntimeAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>
      adapter(MakeConnectionConfig(), MakeCredentials());
  FakeRuntime runtime;

  adapter.BindRuntime(runtime);
  adapter.PushOrderResponseForTest(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kAck,
      .local_order_id = 11,
  });
  adapter.PushOrderResponseForTest(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kCancelRejected,
      .local_order_id = 12,
      .error_label_hash = 12345,
  });

  ASSERT_EQ(runtime.responses.size(), 2U);
  EXPECT_EQ(runtime.callback_thread, std::this_thread::get_id());
  EXPECT_EQ(runtime.responses[0].kind, core::OrderResponseKind::kAck);
  EXPECT_EQ(runtime.responses[0].local_order_id, 11U);
  EXPECT_EQ(runtime.responses[1].kind,
            core::OrderResponseKind::kCancelRejected);
  EXPECT_EQ(runtime.responses[1].local_order_id, 12U);
}

TEST(OrderSessionRuntimeAdapterTest, LoginReadyCallbackUpdatesReadyFlag) {
  OrderSessionRuntimeAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>
      adapter(MakeConnectionConfig(), MakeCredentials());
  EXPECT_FALSE(adapter.Ready());

  adapter.MarkLoginReadyForTest();

  EXPECT_TRUE(adapter.Ready());

  adapter.MarkLoginNotReadyForTest();

  EXPECT_FALSE(adapter.Ready());
}

TEST(OrderSessionRuntimeAdapterTest,
     CanBackStrategyOrderManagerWithoutConnectingGate) {
  using Adapter =
      OrderSessionRuntimeAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>;
  Adapter adapter(MakeConnectionConfig(), MakeCredentials());
  core::OrderManager<Adapter> order_manager(adapter, 4, 3);

  const core::OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(core::OrderCreateRequest{
          .exchange = Exchange::kGate,
          .symbol_id = 7,
          .symbol = "BTC_USDT",
          .side = OrderSide::kBuy,
          .time_in_force = TimeInForce::kGoodTillCancel,
          .quantity = 1,
          .quantity_text = "1",
          .price_text = "81000",
          .reduce_only = false,
      });

  EXPECT_EQ(placed.status, core::OrderPlaceStatus::kSessionRejected);
  EXPECT_NE(placed.local_order_id, 0U);
}

TEST(OrderSessionRuntimeAdapterTest, AdapterIsMoveOnly) {
  using Adapter =
      OrderSessionRuntimeAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>;

  static_assert(!std::is_copy_constructible_v<Adapter>);
  static_assert(!std::is_copy_assignable_v<Adapter>);
  static_assert(std::is_move_constructible_v<Adapter>);
  static_assert(std::is_move_assignable_v<Adapter>);

  Adapter adapter(MakeConnectionConfig(), MakeCredentials());
  adapter.MarkLoginReadyForTest();

  Adapter moved(std::move(adapter));

  EXPECT_TRUE(moved.Ready());
}

}  // namespace
}  // namespace aquila::gate
