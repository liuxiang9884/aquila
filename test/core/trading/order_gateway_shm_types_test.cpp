#include "core/trading/order_gateway_shm_types.h"

#include <type_traits>

#include <gtest/gtest.h>

namespace aquila::core {
namespace {

TEST(OrderGatewayShmTypesTest, PayloadTypesArePodLike) {
  EXPECT_TRUE(std::is_standard_layout_v<OrderPlaceRequest>);
  EXPECT_TRUE(std::is_trivially_copyable_v<OrderPlaceRequest>);
  EXPECT_TRUE(std::is_standard_layout_v<OrderCancelRequest>);
  EXPECT_TRUE(std::is_trivially_copyable_v<OrderCancelRequest>);
  EXPECT_TRUE(std::is_standard_layout_v<OrderGatewayCommand>);
  EXPECT_TRUE(std::is_trivially_copyable_v<OrderGatewayCommand>);
  EXPECT_TRUE(std::is_standard_layout_v<OrderGatewayEvent>);
  EXPECT_TRUE(std::is_trivially_copyable_v<OrderGatewayEvent>);
  EXPECT_TRUE(std::is_standard_layout_v<OrderGatewayQueueDescriptor>);
  EXPECT_TRUE(std::is_trivially_copyable_v<OrderGatewayQueueDescriptor>);
  EXPECT_TRUE(std::is_standard_layout_v<OrderGatewayShmHeader>);
  EXPECT_TRUE(std::is_trivially_copyable_v<OrderGatewayShmHeader>);
}

TEST(OrderGatewayShmTypesTest, ConstantsMatchDesign) {
  EXPECT_EQ(kOrderGatewayShmMagic, 0x41514F47U);
  EXPECT_EQ(kOrderGatewayShmVersion, 4U);
  EXPECT_EQ(kMaxOrderGatewayRoutes, 16U);
  EXPECT_EQ(kOrderSymbolBytes, 32U);
  EXPECT_EQ(sizeof(OrderPlaceRequest), 88U);
  EXPECT_EQ(sizeof(OrderCancelRequest), 32U);
  EXPECT_EQ(sizeof(OrderGatewayCommand), 112U);
  EXPECT_EQ(sizeof(OrderGatewayEvent), 144U);
}

TEST(OrderGatewayShmTypesTest, PlaceAndCancelUseExactRequestPayloads) {
  OrderGatewayCommand place{};
  place.kind = OrderGatewayCommandKind::kPlace;
  place.payload.place.local_order_id = 11;
  place.payload.place.group_id = 17;
  place.payload.place.price = 60123.4;
  place.payload.place.quantity = 2.0;
  place.payload.place.price_decimal_places = 1;
  place.payload.place.quantity_decimal_places = 0;

  EXPECT_EQ(place.payload.place.local_order_id, 11U);
  EXPECT_EQ(place.payload.place.group_id, 17U);
  EXPECT_EQ(OrderGatewayCommandGroupId(place), 17U);
  EXPECT_DOUBLE_EQ(place.payload.place.price, 60123.4);
  EXPECT_DOUBLE_EQ(place.payload.place.quantity, 2.0);
  EXPECT_EQ(place.payload.place.price_decimal_places, 1U);
  EXPECT_EQ(place.payload.place.quantity_decimal_places, 0U);

  OrderGatewayCommand cancel{};
  cancel.kind = OrderGatewayCommandKind::kCancel;
  cancel.payload.cancel.local_order_id = 11;
  cancel.payload.cancel.parent_id = 7;
  cancel.payload.cancel.group_id = 17;
  cancel.payload.cancel.gateway_route_id = 3;

  EXPECT_EQ(cancel.payload.cancel.local_order_id, 11U);
  EXPECT_EQ(cancel.payload.cancel.parent_id, 7U);
  EXPECT_EQ(cancel.payload.cancel.group_id, 17U);
  EXPECT_EQ(OrderGatewayCommandGroupId(cancel), 17U);
  EXPECT_EQ(cancel.payload.cancel.gateway_route_id, 3U);
}

TEST(OrderGatewayShmTypesTest, InternalOrderIdCommandRetainsRoute) {
  OrderGatewayCommand command{};
  command.kind = OrderGatewayCommandKind::kCacheExchangeOrderId;
  command.payload.order_id = OrderGatewayOrderIdCommand{
      .local_order_id = 11,
      .exchange_order_id = 22,
      .gateway_route_id = 3,
  };

  EXPECT_EQ(OrderGatewayCommandLocalOrderId(command), 11U);
  EXPECT_EQ(OrderGatewayCommandExchangeOrderId(command), 22U);
  EXPECT_EQ(OrderGatewayCommandRouteId(command), 3U);
}

TEST(OrderGatewayShmTypesTest, HeaderDefaultsMatchDesign) {
  const OrderGatewayShmHeader header{};

  EXPECT_EQ(header.magic, kOrderGatewayShmMagic);
  EXPECT_EQ(header.version, kOrderGatewayShmVersion);
  EXPECT_EQ(header.header_size, sizeof(OrderGatewayShmHeader));
  EXPECT_EQ(header.route_count, 0U);
  EXPECT_EQ(header.command_queue_capacity, 0U);
  EXPECT_EQ(header.event_queue_capacity, 0U);
  EXPECT_EQ(header.startup_ready_timeout_s, 30U);
  for (std::uint16_t route = 0; route < kMaxOrderGatewayRoutes; ++route) {
    EXPECT_EQ(header.route_states[route],
              static_cast<std::uint32_t>(OrderGatewayRouteState::kUnknown));
  }
}

}  // namespace
}  // namespace aquila::core
