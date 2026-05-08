#include "tools/gate/strategy_runtime_adapter.h"

#include <cstdint>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/strategy/order_manager.h"
#include "core/strategy/order_types.h"
#include "core/websocket/types.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::tools::gate_strategy_runtime {
namespace {

struct CollectingHandler {
  std::vector<strategy::OrderResponseEvent> responses;

  void OnOrderResponse(const strategy::OrderResponseEvent& event) {
    responses.push_back(event);
  }
};

struct CallableHandler {
  std::vector<strategy::OrderResponseEvent> responses;

  void operator()(const strategy::OrderResponseEvent& event) {
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

TEST(GateStrategyRuntimeAdapterTest, ConvertsGateResponsesToStrategyEvents) {
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

TEST(GateStrategyRuntimeAdapterTest, ConvertsEveryGateResponseKind) {
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

TEST(GateStrategyRuntimeAdapterTest,
     QueuesConvertedResponsesUntilRuntimePollsThem) {
  GateOrderSessionAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>
      adapter(MakeConnectionConfig(), MakeCredentials());
  CollectingHandler handler;

  adapter.PushOrderResponseForTest(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kAck,
      .local_order_id = 11,
  });
  adapter.PushOrderResponseForTest(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kCancelRejected,
      .local_order_id = 12,
      .error_label_hash = 12345,
  });

  EXPECT_EQ(adapter.PollOrderResponses(handler), 2U);
  ASSERT_EQ(handler.responses.size(), 2U);
  EXPECT_EQ(handler.responses[0].kind, strategy::OrderResponseKind::kAck);
  EXPECT_EQ(handler.responses[0].local_order_id, 11U);
  EXPECT_EQ(handler.responses[1].kind,
            strategy::OrderResponseKind::kCancelRejected);
  EXPECT_EQ(handler.responses[1].local_order_id, 12U);
  EXPECT_EQ(handler.responses[1].error_label_hash, 12345U);

  EXPECT_EQ(adapter.PollOrderResponses(handler), 0U);
  EXPECT_EQ(handler.responses.size(), 2U);
}

TEST(GateStrategyRuntimeAdapterTest, PollSupportsCallableHandlers) {
  GateOrderSessionAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>
      adapter(MakeConnectionConfig(), MakeCredentials());
  CallableHandler handler;

  adapter.PushOrderResponseForTest(gate::OrderResponse{
      .kind = gate::OrderResponseKind::kRejected,
      .local_order_id = 21,
      .error_label_hash = 7,
  });

  EXPECT_EQ(adapter.PollOrderResponses(handler), 1U);
  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, strategy::OrderResponseKind::kRejected);
  EXPECT_EQ(handler.responses[0].local_order_id, 21U);
  EXPECT_EQ(handler.responses[0].error_label_hash, 7U);
}

TEST(GateStrategyRuntimeAdapterTest, LoginReadyCallbackUpdatesReadyFlag) {
  GateOrderSessionAdapter<gate::OrderSessionDefaultPlainWebSocketPolicy>
      adapter(MakeConnectionConfig(), MakeCredentials());
  EXPECT_FALSE(adapter.Ready());
  EXPECT_FALSE(adapter.Running());

  adapter.MarkLoginReadyForTest();

  EXPECT_TRUE(adapter.Ready());
  EXPECT_TRUE(adapter.Running());
}

TEST(GateStrategyRuntimeAdapterTest,
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

TEST(GateStrategyRuntimeAdapterTest, AdapterIsMoveOnly) {
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
}  // namespace aquila::tools::gate_strategy_runtime
