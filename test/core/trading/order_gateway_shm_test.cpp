#include "core/trading/order_gateway_shm.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace aquila::core {
namespace {

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

  std::string shm_name;
};

std::string UniqueShmName(std::string_view suffix) {
  return fmt::format("/aquila_order_gateway_shm_test_{}_{}", ::getpid(),
                     suffix);
}

OrderGatewayShmConfig MakeCreateConfig(std::string_view suffix) {
  return OrderGatewayShmConfig{
      .shm_name = UniqueShmName(suffix),
      .create = true,
      .remove_existing = true,
      .route_count = 4,
      .command_queue_capacity = 4096,
      .event_queue_capacity = 8192,
      .startup_ready_timeout_s = 30,
  };
}

OrderGatewayShmConfig MakeOpenConfig(
    const OrderGatewayShmConfig& create_config) {
  OrderGatewayShmConfig config = create_config;
  config.create = false;
  config.remove_existing = false;
  return config;
}

OrderGatewayCommand MakeCommand(std::uint64_t command_seq) {
  OrderGatewayCommand command{};
  command.command_seq = command_seq;
  command.parent_id = 1000 + command_seq;
  command.local_order_id = 2000 + command_seq;
  command.route_id = 2;
  command.kind = OrderGatewayCommandKind::kPlace;
  command.symbol_id = 42;
  command.quantity = 3.5;
  return command;
}

OrderGatewayEvent MakeEvent(std::uint64_t event_seq) {
  OrderGatewayEvent event{};
  event.event_seq = event_seq;
  event.command_seq = 7000 + event_seq;
  event.local_order_id = 8000 + event_seq;
  event.route_id = 3;
  event.kind = OrderGatewayEventKind::kOrderResponse;
  event.response_kind = OrderResponseKind::kAck;
  return event;
}

TEST(OrderGatewayShmTest, CreateInitializesHeaderAndQueueDescriptors) {
  const OrderGatewayShmConfig config = MakeCreateConfig("create_init");
  ShmCleanup cleanup(config.shm_name);

  auto create_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(create_result.ok) << create_result.error;

  const OrderGatewayShmHeader& header = create_result.value.header();
  EXPECT_EQ(header.magic, kOrderGatewayShmMagic);
  EXPECT_EQ(header.version, kOrderGatewayShmVersion);
  EXPECT_EQ(header.header_size, sizeof(OrderGatewayShmHeader));
  EXPECT_EQ(header.route_count, 4U);
  EXPECT_EQ(header.command_queue_capacity, 4096U);
  EXPECT_EQ(header.event_queue_capacity, 8192U);
  EXPECT_EQ(header.startup_ready_timeout_s, 30U);

  for (std::uint16_t route = 0; route < header.route_count; ++route) {
    EXPECT_EQ(LoadOrderGatewayRouteState(create_result.value.header(), route),
              OrderGatewayRouteState::kUnknown);
    EXPECT_EQ(header.command_queue_descriptors[route].capacity, 4096U);
    EXPECT_EQ(header.command_queue_descriptors[route].slot_size,
              sizeof(OrderGatewayCommand));
    EXPECT_EQ(header.event_queue_descriptors[route].capacity, 8192U);
    EXPECT_EQ(header.event_queue_descriptors[route].slot_size,
              sizeof(OrderGatewayEvent));
  }
}

TEST(OrderGatewayShmTest, OpenAttachesExistingShmByName) {
  const OrderGatewayShmConfig config = MakeCreateConfig("open_existing");
  ShmCleanup cleanup(config.shm_name);

  auto create_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(create_result.ok) << create_result.error;
  ASSERT_TRUE(create_result.value.CommandQueue(1).TryPush(MakeCommand(11)));

  auto open_result = OrderGatewayShmManager::Open(MakeOpenConfig(config));
  ASSERT_TRUE(open_result.ok) << open_result.error;

  OrderGatewayCommand popped{};
  EXPECT_TRUE(open_result.value.CommandQueue(1).TryPop(&popped));
  EXPECT_EQ(popped.command_seq, 11U);
  EXPECT_EQ(open_result.value.header().route_count, 4U);
}

TEST(OrderGatewayShmTest, OpenByNameDoesNotRequireExpectedLayoutFields) {
  OrderGatewayShmConfig config = MakeCreateConfig("open_by_name_only");
  config.startup_ready_timeout_s = 31;
  ShmCleanup cleanup(config.shm_name);

  auto create_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(create_result.ok) << create_result.error;

  auto open_result = OrderGatewayShmManager::Open(OrderGatewayShmConfig{
      .shm_name = config.shm_name,
      .create = false,
  });
  ASSERT_TRUE(open_result.ok) << open_result.error;
  EXPECT_EQ(open_result.value.header().startup_ready_timeout_s, 31U);
}

TEST(OrderGatewayShmTest, OpenRejectsMismatchedExpectedLayout) {
  const OrderGatewayShmConfig config = MakeCreateConfig("mismatch_expected");
  ShmCleanup cleanup(config.shm_name);

  auto create_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(create_result.ok) << create_result.error;

  OrderGatewayShmConfig open_config = MakeOpenConfig(config);
  open_config.route_count = 3;
  auto route_mismatch_result = OrderGatewayShmManager::Open(open_config);
  EXPECT_FALSE(route_mismatch_result.ok);

  open_config = MakeOpenConfig(config);
  open_config.command_queue_capacity = 1024;
  auto command_capacity_mismatch_result =
      OrderGatewayShmManager::Open(open_config);
  EXPECT_FALSE(command_capacity_mismatch_result.ok);

  open_config = MakeOpenConfig(config);
  open_config.event_queue_capacity = 4096;
  auto event_capacity_mismatch_result =
      OrderGatewayShmManager::Open(open_config);
  EXPECT_FALSE(event_capacity_mismatch_result.ok);
}

TEST(OrderGatewayShmTest, OpenRejectsCorruptedQueueDescriptorLayout) {
  const OrderGatewayShmConfig config = MakeCreateConfig("corrupt_descriptor");
  ShmCleanup cleanup(config.shm_name);

  auto create_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(create_result.ok) << create_result.error;
  create_result.value.header().event_queue_descriptors[1].offset =
      create_result.value.header().command_queue_descriptors[1].offset;

  auto open_result = OrderGatewayShmManager::Open(MakeOpenConfig(config));
  EXPECT_FALSE(open_result.ok);
}

TEST(OrderGatewayShmTest, RejectsInvalidRouteCount) {
  OrderGatewayShmConfig zero_config = MakeCreateConfig("zero_route");
  zero_config.route_count = 0;
  ShmCleanup zero_cleanup(zero_config.shm_name);
  auto zero_result = OrderGatewayShmManager::Create(zero_config);
  EXPECT_FALSE(zero_result.ok);

  OrderGatewayShmConfig too_many_config = MakeCreateConfig("too_many_routes");
  too_many_config.route_count = kMaxOrderGatewayRoutes + 1;
  ShmCleanup too_many_cleanup(too_many_config.shm_name);
  auto too_many_result = OrderGatewayShmManager::Create(too_many_config);
  EXPECT_FALSE(too_many_result.ok);
}

TEST(OrderGatewayShmTest, CommandQueuePushPopOneCommand) {
  const OrderGatewayShmConfig config = MakeCreateConfig("command_push_pop");
  ShmCleanup cleanup(config.shm_name);

  auto create_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(create_result.ok) << create_result.error;

  const OrderGatewayCommand expected = MakeCommand(21);
  ASSERT_TRUE(create_result.value.CommandQueue(2).TryPush(expected));

  OrderGatewayCommand actual{};
  ASSERT_TRUE(create_result.value.CommandQueue(2).TryPop(&actual));
  EXPECT_EQ(actual.command_seq, expected.command_seq);
  EXPECT_EQ(actual.parent_id, expected.parent_id);
  EXPECT_EQ(actual.local_order_id, expected.local_order_id);
  EXPECT_EQ(actual.route_id, expected.route_id);
  EXPECT_FALSE(create_result.value.CommandQueue(2).TryPop(&actual));
}

TEST(OrderGatewayShmTest, EventQueuePushPopOneEvent) {
  const OrderGatewayShmConfig config = MakeCreateConfig("event_push_pop");
  ShmCleanup cleanup(config.shm_name);

  auto create_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(create_result.ok) << create_result.error;

  const OrderGatewayEvent expected = MakeEvent(31);
  ASSERT_TRUE(create_result.value.EventQueue(3).TryPush(expected));

  OrderGatewayEvent actual{};
  ASSERT_TRUE(create_result.value.EventQueue(3).TryPop(&actual));
  EXPECT_EQ(actual.event_seq, expected.event_seq);
  EXPECT_EQ(actual.command_seq, expected.command_seq);
  EXPECT_EQ(actual.local_order_id, expected.local_order_id);
  EXPECT_EQ(actual.route_id, expected.route_id);
  EXPECT_EQ(actual.kind, expected.kind);
  EXPECT_FALSE(create_result.value.EventQueue(3).TryPop(&actual));
}

TEST(OrderGatewayShmTest, FullCommandQueueRejectsWithoutOverwritingUnreadData) {
  OrderGatewayShmConfig config = MakeCreateConfig("command_full");
  config.command_queue_capacity = 2;
  config.event_queue_capacity = 2;
  ShmCleanup cleanup(config.shm_name);

  auto create_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(create_result.ok) << create_result.error;
  auto queue = create_result.value.CommandQueue(0);

  EXPECT_TRUE(queue.TryPush(MakeCommand(1)));
  EXPECT_TRUE(queue.TryPush(MakeCommand(2)));
  EXPECT_FALSE(queue.TryPush(MakeCommand(3)));

  OrderGatewayCommand actual{};
  ASSERT_TRUE(queue.TryPop(&actual));
  EXPECT_EQ(actual.command_seq, 1U);
  ASSERT_TRUE(queue.TryPop(&actual));
  EXPECT_EQ(actual.command_seq, 2U);
  EXPECT_FALSE(queue.TryPop(&actual));
}

}  // namespace
}  // namespace aquila::core
