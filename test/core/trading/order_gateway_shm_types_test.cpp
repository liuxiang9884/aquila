#include "core/trading/order_gateway_shm_types.h"

#include <type_traits>

#include <gtest/gtest.h>

namespace aquila::core {
namespace {

TEST(OrderGatewayShmTypesTest, PayloadTypesArePodLike) {
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
  EXPECT_EQ(kOrderGatewayShmVersion, 3U);
  EXPECT_EQ(kMaxOrderGatewayRoutes, 16U);
  EXPECT_EQ(kOrderGatewaySymbolBytes, 32U);
  EXPECT_EQ(kOrderGatewayQuantityTextBytes, 32U);
  EXPECT_EQ(kOrderGatewayPriceTextBytes, 32U);
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
