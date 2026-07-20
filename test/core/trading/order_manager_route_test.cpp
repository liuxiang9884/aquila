#include <cstdint>
#include <vector>

#include "core/trading/order_manager.h"
#include "core/trading/order_types.h"
#include "gtest/gtest.h"

namespace aquila::core {
namespace {

enum class FakeSendStatus : std::uint8_t {
  kOk,
  kRejected,
};

struct FakeSendResult {
  FakeSendStatus status{FakeSendStatus::kOk};
  std::int64_t send_local_ns{0};
};

class CapturingGateway {
 public:
  FakeSendResult PlaceOrder(const OrderPlaceRequest& order) noexcept {
    placed_routes.push_back(order.gateway_route_id);
    placed_ids.push_back(order.local_order_id);
    placed_parent_ids.push_back(order.parent_id);
    return {.status = FakeSendStatus::kOk, .send_local_ns = 123};
  }

  FakeSendResult CancelOrder(const OrderCancelRequest& order) noexcept {
    cancelled_routes.push_back(order.gateway_route_id);
    cancelled_ids.push_back(order.local_order_id);
    return {.status = FakeSendStatus::kOk, .send_local_ns = 0};
  }

  std::vector<std::uint16_t> placed_routes;
  std::vector<std::uint64_t> placed_ids;
  std::vector<std::uint64_t> placed_parent_ids;
  std::vector<std::uint16_t> cancelled_routes;
  std::vector<std::uint64_t> cancelled_ids;
};

OrderPlaceRequest MakeRequest(std::uint16_t route_id) noexcept {
  OrderPlaceRequest request;
  request.symbol_id = 1;
  request.price = 50000.0;
  request.quantity = 1.0;
  request.gateway_route_id = route_id;
  SetOrderSymbol(&request, "BTC_USDT");
  return request;
}

TEST(OrderManagerRouteTest, CopiesExplicitGatewayRouteToStrategyOrder) {
  CapturingGateway gateway;
  OrderManager<CapturingGateway> manager(gateway, 8, 7);

  const OrderPlaceResult placed = manager.PlaceOrder(MakeRequest(3));

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(gateway.placed_routes.size(), 1U);
  EXPECT_EQ(gateway.placed_routes[0], 3U);
  EXPECT_EQ(
      manager.FindOrder(placed.local_order_id)->place_request.gateway_route_id,
      3U);
}

TEST(OrderManagerRouteTest, DefaultsGatewayRouteToAuto) {
  CapturingGateway gateway;
  OrderManager<CapturingGateway> manager(gateway, 8, 7);

  OrderPlaceRequest request = MakeRequest(kAutoGatewayRoute);
  const OrderPlaceResult placed = manager.PlaceOrder(request);

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(gateway.placed_routes.size(), 1U);
  EXPECT_EQ(gateway.placed_routes[0], kAutoGatewayRoute);
  EXPECT_EQ(
      manager.FindOrder(placed.local_order_id)->place_request.gateway_route_id,
      kAutoGatewayRoute);
}

TEST(OrderManagerRouteTest, ChildOrdersGetUniqueLocalIdsWithRoutes) {
  CapturingGateway gateway;
  OrderManager<CapturingGateway> manager(gateway, 8, 7);

  const OrderPlaceResult first = manager.PlaceOrder(MakeRequest(0));
  const OrderPlaceResult second = manager.PlaceOrder(MakeRequest(3));

  ASSERT_EQ(first.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(second.status, OrderPlaceStatus::kOk);
  EXPECT_NE(first.local_order_id, second.local_order_id);
  ASSERT_EQ(gateway.placed_routes.size(), 2U);
  EXPECT_EQ(gateway.placed_routes[0], 0U);
  EXPECT_EQ(gateway.placed_routes[1], 3U);
}

TEST(OrderManagerRouteTest, CopiesParentIdToStrategyOrder) {
  CapturingGateway gateway;
  OrderManager<CapturingGateway> manager(gateway, 8, 7);
  OrderPlaceRequest request = MakeRequest(1);
  request.parent_id = 9001;

  const OrderPlaceResult placed = manager.PlaceOrder(request);

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(gateway.placed_parent_ids.size(), 1U);
  EXPECT_EQ(gateway.placed_parent_ids[0], 9001U);
  EXPECT_EQ(manager.FindOrder(placed.local_order_id)->place_request.parent_id,
            9001U);
}

TEST(OrderManagerRouteTest, CancelPreservesStoredGatewayRoute) {
  CapturingGateway gateway;
  OrderManager<CapturingGateway> manager(gateway, 8, 7);

  const OrderPlaceResult placed = manager.PlaceOrder(MakeRequest(2));
  const OrderCancelResult cancelled = manager.CancelOrder(
      OrderCancelRequest{.local_order_id = placed.local_order_id});

  ASSERT_EQ(cancelled.status, OrderCancelStatus::kOk);
  ASSERT_EQ(gateway.cancelled_routes.size(), 1U);
  EXPECT_EQ(gateway.cancelled_routes[0], 2U);
  EXPECT_EQ(gateway.cancelled_ids[0], placed.local_order_id);
}

}  // namespace
}  // namespace aquila::core
