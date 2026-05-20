#include "tools/gate/trading_runtime_adapter.h"

#include <array>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/strategy/order_manager.h"
#include "core/strategy/order_types.h"
#include "core/websocket/types.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::tools::gate_trading_runtime {
namespace {

struct FakeRuntime {
  std::vector<strategy::OrderResponseEvent> responses;
  std::thread::id callback_thread;

  void OnOrderResponse(const strategy::OrderResponseEvent& event) noexcept {
    callback_thread = std::this_thread::get_id();
    responses.push_back(event);
  }
};

std::array<detail::GateErrorResponseLogRecordForTest, 4> g_logged_errors{};
std::array<std::size_t, 4> g_response_count_at_log{};
std::size_t g_logged_error_count{0};
const FakeRuntime* g_runtime_seen_by_log{nullptr};

void CaptureGateErrorResponseLogForTest(
    const detail::GateErrorResponseLogRecordForTest& record) noexcept {
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

void ResetGateErrorResponseLogCapture() noexcept {
  g_logged_errors = {};
  g_response_count_at_log = {};
  g_logged_error_count = 0;
  g_runtime_seen_by_log = nullptr;
  detail::SetGateErrorResponseLogObserverForTest(nullptr);
}

class GateErrorResponseLogCaptureGuard {
 public:
  explicit GateErrorResponseLogCaptureGuard(
      const FakeRuntime& runtime) noexcept {
    ResetGateErrorResponseLogCapture();
    g_runtime_seen_by_log = &runtime;
    detail::SetGateErrorResponseLogObserverForTest(
        CaptureGateErrorResponseLogForTest);
  }

  ~GateErrorResponseLogCaptureGuard() noexcept {
    ResetGateErrorResponseLogCapture();
  }

  GateErrorResponseLogCaptureGuard(const GateErrorResponseLogCaptureGuard&) =
      delete;
  GateErrorResponseLogCaptureGuard& operator=(
      const GateErrorResponseLogCaptureGuard&) = delete;
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

TEST(GateTradingRuntimeAdapterTest,
     LogsGateErrorResponsesBeforeRuntimeDispatch) {
  GateOrderSessionAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>
      adapter(MakeConnectionConfig(), MakeCredentials());
  FakeRuntime runtime;
  GateErrorResponseLogCaptureGuard log_capture(runtime);

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
  });
  adapter.PushOrderResponseForTest(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kCancelRejected,
      .local_order_id = 13,
      .exchange_order_id = 333,
      .request_sequence = 30,
      .http_status = 404,
      .error_label_hash = 444,
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
  EXPECT_EQ(g_response_count_at_log[0], 1U);
  EXPECT_EQ(g_logged_errors[1].kind, gate::OrderResponseKind::kCancelRejected);
  EXPECT_EQ(g_logged_errors[1].local_order_id, 13U);
  EXPECT_EQ(g_logged_errors[1].exchange_order_id, 333U);
  EXPECT_EQ(g_logged_errors[1].request_sequence, 30U);
  EXPECT_EQ(g_logged_errors[1].http_status, 404);
  EXPECT_EQ(g_logged_errors[1].error_label_hash, 444U);
  EXPECT_EQ(g_response_count_at_log[1], 2U);
}

TEST(GateTradingRuntimeAdapterTest, ConvertsGateResponsesToStrategyEvents) {
  const gate::OrderResponse accepted{
      .kind = gate::OrderResponseKind::kAccepted,
      .local_order_id = 0x0400000000000007ULL,
      .exchange_order_id = 36028827892199865ULL,
      .request_sequence = 42,
      .http_status = 200,
      .error_label_hash = 99,
  };

  const strategy::OrderResponseEvent event =
      ToStrategyOrderResponseEvent(accepted);

  EXPECT_EQ(event.kind, strategy::OrderResponseKind::kAccepted);
  EXPECT_EQ(event.local_order_id, accepted.local_order_id);
  EXPECT_EQ(event.exchange_order_id, accepted.exchange_order_id);
}

TEST(GateTradingRuntimeAdapterTest, ConvertsEveryGateResponseKind) {
  EXPECT_EQ(ToStrategyOrderResponseKind(gate::OrderResponseKind::kAck),
            strategy::OrderResponseKind::kAck);
  EXPECT_EQ(ToStrategyOrderResponseKind(gate::OrderResponseKind::kAccepted),
            strategy::OrderResponseKind::kAccepted);
  EXPECT_EQ(ToStrategyOrderResponseKind(gate::OrderResponseKind::kRejected),
            strategy::OrderResponseKind::kRejected);
  EXPECT_EQ(
      ToStrategyOrderResponseKind(gate::OrderResponseKind::kCancelAccepted),
      strategy::OrderResponseKind::kCancelAccepted);
  EXPECT_EQ(
      ToStrategyOrderResponseKind(gate::OrderResponseKind::kCancelRejected),
      strategy::OrderResponseKind::kCancelRejected);
}

TEST(GateTradingRuntimeAdapterTest,
     BindRuntimeDispatchesOrderResponsesSynchronously) {
  GateOrderSessionAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>
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
  EXPECT_EQ(runtime.responses[0].kind, strategy::OrderResponseKind::kAck);
  EXPECT_EQ(runtime.responses[0].local_order_id, 11U);
  EXPECT_EQ(runtime.responses[1].kind,
            strategy::OrderResponseKind::kCancelRejected);
  EXPECT_EQ(runtime.responses[1].local_order_id, 12U);
}

TEST(GateTradingRuntimeAdapterTest, LoginReadyCallbackUpdatesReadyFlag) {
  GateOrderSessionAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>
      adapter(MakeConnectionConfig(), MakeCredentials());
  EXPECT_FALSE(adapter.Ready());

  adapter.MarkLoginReadyForTest();

  EXPECT_TRUE(adapter.Ready());

  adapter.MarkLoginNotReadyForTest();

  EXPECT_FALSE(adapter.Ready());
}

TEST(GateTradingRuntimeAdapterTest,
     CanBackStrategyOrderManagerWithoutConnectingGate) {
  using Adapter =
      GateOrderSessionAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>;
  Adapter adapter(MakeConnectionConfig(), MakeCredentials());
  strategy::OrderManager<Adapter> order_manager(adapter, 4, 3);

  const strategy::OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(strategy::OrderCreateRequest{
          .exchange = Exchange::kGate,
          .symbol_id = 7,
          .symbol = "BTC_USDT",
          .side = OrderSide::kBuy,
          .time_in_force = TimeInForce::kGoodTillCancel,
          .quantity = 1,
          .price_text = "81000",
          .reduce_only = false,
      });

  EXPECT_EQ(placed.status, strategy::OrderPlaceStatus::kSessionRejected);
  EXPECT_NE(placed.local_order_id, 0U);
}

TEST(GateTradingRuntimeAdapterTest, AdapterIsMoveOnly) {
  using Adapter =
      GateOrderSessionAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>;

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
}  // namespace aquila::tools::gate_trading_runtime
