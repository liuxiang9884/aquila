#include "tools/gate/trading_runtime_adapter.h"

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
  EXPECT_EQ(event.error_label_hash, accepted.error_label_hash);
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
  EXPECT_EQ(runtime.responses[1].error_label_hash, 12345U);
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
