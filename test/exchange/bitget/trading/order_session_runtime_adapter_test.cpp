#include "exchange/bitget/trading/order_session_runtime_adapter.h"

#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/trading/order_manager.h"
#include "core/trading/order_types.h"
#include "core/websocket/types.h"

namespace aquila::bitget {
namespace {

struct FakeRuntime {
  std::vector<core::OrderResponseEvent> responses;
  std::thread::id callback_thread;

  void OnOrderResponse(const core::OrderResponseEvent& event) noexcept {
    responses.push_back(event);
    callback_thread = std::this_thread::get_id();
  }
};

websocket::ConnectionConfig MakeConnectionConfig() {
  websocket::ConnectionConfig config;
  config.host = "127.0.0.1";
  config.target = "/v3/ws/private";
  config.port = "1";
  config.enable_tls = false;
  return config;
}

LoginCredentials MakeCredentials() {
  return LoginCredentials{.api_key = "test_key",
                          .api_secret = "test_secret",
                          .passphrase = "test_passphrase"};
}

TEST(BitgetOrderSessionRuntimeAdapterTest, ConvertsEveryResponseKind) {
  EXPECT_EQ(ToCoreOrderResponseKind(OrderResponseKind::kAck),
            core::OrderResponseKind::kAck);
  EXPECT_EQ(ToCoreOrderResponseKind(OrderResponseKind::kRejected),
            core::OrderResponseKind::kRejected);
  EXPECT_EQ(ToCoreOrderResponseKind(OrderResponseKind::kCancelRejected),
            core::OrderResponseKind::kCancelRejected);
  EXPECT_EQ(ToCoreOrderResponseKind(OrderResponseKind::kUnknownResult),
            core::OrderResponseKind::kUnknownResult);
}

TEST(BitgetOrderSessionRuntimeAdapterTest, PreservesCoreEventFields) {
  const OrderResponse response{
      .kind = OrderResponseKind::kAck,
      .request_type = OrderRequestType::kPlaceOrder,
      .local_order_id = 0x0400000000000007ULL,
      .parent_id = 88,
      .group_id = 77,
      .exchange_order_id = 9988,
      .route_id = 3,
      .local_receive_ns = 123456789,
      .exchange_ns = 1750034397076000000LL,
  };

  const core::OrderResponseEvent event = ToCoreOrderResponseEvent(response);

  EXPECT_EQ(event.kind, core::OrderResponseKind::kAck);
  EXPECT_EQ(event.local_order_id, response.local_order_id);
  EXPECT_EQ(event.parent_id, response.parent_id);
  EXPECT_EQ(event.group_id, response.group_id);
  EXPECT_EQ(event.exchange_order_id, response.exchange_order_id);
  EXPECT_EQ(event.route_id, response.route_id);
  EXPECT_EQ(event.local_receive_ns, response.local_receive_ns);
  EXPECT_EQ(event.exchange_ns, response.exchange_ns);
}

TEST(BitgetOrderSessionRuntimeAdapterTest,
     DispatchesSynchronouslyAndTracksReady) {
  using Adapter =
      OrderSessionRuntimeAdapter<OrderSessionDefaultPlainWebSocketPolicy>;
  Adapter adapter(MakeConnectionConfig(), MakeCredentials());
  FakeRuntime runtime;
  adapter.BindRuntime(runtime);

  EXPECT_FALSE(adapter.Ready());
  adapter.MarkLoginReadyForTest();
  EXPECT_TRUE(adapter.Ready());

  adapter.PushOrderResponseForTest(OrderResponse{
      .kind = OrderResponseKind::kUnknownResult,
      .local_order_id = 11,
      .error_code = 40010,
  });

  ASSERT_EQ(runtime.responses.size(), 1U);
  EXPECT_EQ(runtime.callback_thread, std::this_thread::get_id());
  EXPECT_EQ(runtime.responses[0].kind, core::OrderResponseKind::kUnknownResult);
  EXPECT_EQ(runtime.responses[0].local_order_id, 11U);

  adapter.MarkLoginNotReadyForTest();
  EXPECT_FALSE(adapter.Ready());
}

struct FakeGateway {
  struct SendResult {
    enum class Status : std::uint8_t { kOk, kFailed };
    Status status{Status::kOk};
    std::int64_t send_local_ns{123};
  };

  SendResult PlaceOrder(const core::OrderPlaceRequest&) noexcept {
    return {};
  }
  SendResult CancelOrder(const core::OrderCancelRequest&) noexcept {
    return {};
  }
};

TEST(BitgetOrderSessionRuntimeAdapterTest,
     OperationAcksDoNotAdvanceOrderLifecycle) {
  FakeGateway gateway;
  core::OrderManager<FakeGateway> manager(gateway, 4, 3);
  core::OrderPlaceRequest request{
      .price = 100000.0,
      .quantity = 0.001,
      .symbol_id = 7,
      .exchange = Exchange::kBitget,
      .side = OrderSide::kBuy,
      .time_in_force = TimeInForce::kGoodTillCancel,
      .quantity_decimal_places = 3,
  };
  core::SetOrderSymbol(&request, "BTCUSDT");
  const core::OrderPlaceResult placed = manager.PlaceLimitOrder(request);
  ASSERT_EQ(placed.status, core::OrderPlaceStatus::kOk);

  manager.OnOrderResponse(ToCoreOrderResponseEvent(OrderResponse{
      .kind = OrderResponseKind::kAck,
      .request_type = OrderRequestType::kPlaceOrder,
      .local_order_id = placed.local_order_id,
  }));
  ASSERT_EQ(manager.FindOrder(placed.local_order_id)->status,
            core::OrderStatus::kSent);

  ASSERT_EQ(manager
                .CancelOrder(core::OrderCancelRequest{
                    .local_order_id = placed.local_order_id})
                .status,
            core::OrderCancelStatus::kOk);
  manager.OnOrderResponse(ToCoreOrderResponseEvent(OrderResponse{
      .kind = OrderResponseKind::kAck,
      .request_type = OrderRequestType::kCancelOrder,
      .local_order_id = placed.local_order_id,
  }));
  EXPECT_EQ(manager.FindOrder(placed.local_order_id)->status,
            core::OrderStatus::kCancelSent);

  manager.OnOrderResponse(ToCoreOrderResponseEvent(OrderResponse{
      .kind = OrderResponseKind::kCancelRejected,
      .request_type = OrderRequestType::kCancelOrder,
      .local_order_id = placed.local_order_id,
  }));
  EXPECT_EQ(manager.FindOrder(placed.local_order_id)->status,
            core::OrderStatus::kSent);
}

TEST(BitgetOrderSessionRuntimeAdapterTest, AdapterIsMoveOnly) {
  using Adapter =
      OrderSessionRuntimeAdapter<OrderSessionDefaultPlainWebSocketPolicy>;

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
}  // namespace aquila::bitget
