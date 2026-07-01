#include "core/trading/order_gateway_client.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/trading/order_gateway_shm.h"
#include "core/trading/order_manager.h"

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
  return fmt::format("/aquila_order_gateway_client_test_{}_{}", ::getpid(),
                     suffix);
}

OrderGatewayShmConfig MakeCreateConfig(std::string_view suffix) {
  return OrderGatewayShmConfig{
      .shm_name = UniqueShmName(suffix),
      .create = true,
      .remove_existing = true,
      .route_count = 4,
      .command_queue_capacity = 4,
      .event_queue_capacity = 8,
      .startup_ready_timeout_s = 1,
  };
}

OrderGatewayClientConfig MakeClientConfig(
    const OrderGatewayShmConfig& create_config,
    OrderGatewayClientOptions options = {}) {
  return OrderGatewayClientConfig{
      .shm_name = create_config.shm_name,
      .route_count = create_config.route_count,
      .command_queue_capacity = create_config.command_queue_capacity,
      .event_queue_capacity = create_config.event_queue_capacity,
      .startup_ready_timeout_s = create_config.startup_ready_timeout_s,
      .options = options,
  };
}

OrderGatewayEvent MakeReadyEvent(std::uint16_t route_id) {
  OrderGatewayEvent event{};
  event.route_id = route_id;
  event.kind = OrderGatewayEventKind::kReady;
  event.ready = 1;
  return event;
}

OrderGatewayEvent MakeNotReadyEvent(std::uint16_t route_id) {
  OrderGatewayEvent event{};
  event.route_id = route_id;
  event.kind = OrderGatewayEventKind::kNotReady;
  event.ready = 0;
  return event;
}

StrategyOrder MakeOrder(std::uint64_t local_order_id, std::uint16_t route_id) {
  StrategyOrder order{};
  order.local_order_id = local_order_id;
  order.exchange = Exchange::kGate;
  order.symbol_id = 42;
  order.symbol = "BTC_USDT";
  order.side = OrderSide::kBuy;
  order.type = OrderType::kLimit;
  order.time_in_force = TimeInForce::kImmediateOrCancel;
  order.quantity = 0.01;
  order.quantity_text = "0.01";
  order.price_text = "65000";
  order.gateway_route_id = route_id;
  return order;
}

class CapturingRuntime {
 public:
  void OnOrderResponse(const OrderResponseEvent& event) noexcept {
    responses.push_back(event);
  }

  std::vector<OrderResponseEvent> responses;
};

class OrderManagerRuntime {
 public:
  explicit OrderManagerRuntime(OrderManager<OrderGatewayClient>& manager)
      : manager_(&manager) {}

  void OnOrderResponse(const OrderResponseEvent& event) noexcept {
    responses.push_back(event);
    manager_->OnOrderResponse(event);
  }

  std::vector<OrderResponseEvent> responses;

 private:
  OrderManager<OrderGatewayClient>* manager_{nullptr};
};

OrderGatewayClient CreateClient(const OrderGatewayShmConfig& config,
                                OrderGatewayClientOptions options = {}) {
  auto client_result =
      OrderGatewayClient::Open(MakeClientConfig(config, options));
  EXPECT_TRUE(client_result.ok) << client_result.error;
  return std::move(client_result.value);
}

TEST(OrderGatewayClientTest, AttachInitializesRoutesNotReady) {
  const OrderGatewayShmConfig config = MakeCreateConfig("attach_not_ready");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;

  OrderGatewayClient client = CreateClient(config);

  EXPECT_EQ(client.route_count(), 4U);
  EXPECT_EQ(client.ready_route_count(), 0U);
  EXPECT_FALSE(client.Ready());
  for (std::uint16_t route = 0; route < client.route_count(); ++route) {
    EXPECT_FALSE(client.RouteReady(route));
  }
}

TEST(OrderGatewayClientTest, ReadyEventsUpdateCountOnlyOnStateTransition) {
  const OrderGatewayShmConfig config = MakeCreateConfig("ready_transition");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(1).TryPush(MakeReadyEvent(1)));
  ASSERT_TRUE(shm.EventQueue(1).TryPush(MakeReadyEvent(1)));
  ASSERT_TRUE(shm.EventQueue(1).TryPush(MakeNotReadyEvent(1)));
  ASSERT_TRUE(shm.EventQueue(1).TryPush(MakeNotReadyEvent(1)));

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;

  EXPECT_EQ(client.PollOrderResponses(runtime), 4U);
  EXPECT_EQ(client.ready_route_count(), 0U);
  EXPECT_FALSE(client.RouteReady(1));
  EXPECT_TRUE(runtime.responses.empty());
  EXPECT_EQ(client.stats().ready_events, 2U);
  EXPECT_EQ(client.stats().not_ready_events, 2U);
}

TEST(OrderGatewayClientTest, StartWaitsUntilAllRoutesAreReady) {
  const OrderGatewayShmConfig config = MakeCreateConfig("start_all_ready");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  for (std::uint16_t route = 0; route < config.route_count; ++route) {
    ASSERT_TRUE(shm.EventQueue(route).TryPush(MakeReadyEvent(route)));
  }

  OrderGatewayClient client = CreateClient(config);

  EXPECT_TRUE(client.Start());
  EXPECT_TRUE(client.Running());
  EXPECT_TRUE(client.Ready());
  EXPECT_EQ(client.ready_route_count(), 4U);
}

TEST(OrderGatewayClientTest, StartAcceptsHeaderReadyStatesWithoutEvents) {
  const OrderGatewayShmConfig config = MakeCreateConfig("start_header_ready");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  for (std::uint16_t route = 0; route < config.route_count; ++route) {
    StoreOrderGatewayRouteState(shm.header(), route,
                                OrderGatewayRouteState::kReady);
  }

  OrderGatewayClient client = CreateClient(config);

  EXPECT_TRUE(client.Start());
  EXPECT_TRUE(client.Running());
  EXPECT_TRUE(client.Ready());
  EXPECT_EQ(client.ready_route_count(), 4U);
}

TEST(OrderGatewayClientTest, StartDoesNotLetStaleReadyEventOverrideStopped) {
  OrderGatewayShmConfig config = MakeCreateConfig("stale_ready_stopped");
  config.startup_ready_timeout_s = 1;
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  for (std::uint16_t route = 0; route < config.route_count; ++route) {
    ASSERT_TRUE(shm.EventQueue(route).TryPush(MakeReadyEvent(route)));
    StoreOrderGatewayRouteState(shm.header(), route,
                                OrderGatewayRouteState::kStopped);
  }

  OrderGatewayClient client = CreateClient(config);

  EXPECT_FALSE(client.Start());
  EXPECT_FALSE(client.Running());
  EXPECT_EQ(client.ready_route_count(), 0U);
  for (std::uint16_t route = 0; route < config.route_count; ++route) {
    EXPECT_FALSE(client.RouteReady(route));
  }
}

TEST(OrderGatewayClientTest, StartFailsAfterConfiguredTimeout) {
  const OrderGatewayShmConfig config = MakeCreateConfig("start_timeout");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(0).TryPush(MakeReadyEvent(0)));

  OrderGatewayClient client = CreateClient(config);

  EXPECT_FALSE(client.Start());
  EXPECT_FALSE(client.Running());
  EXPECT_EQ(client.ready_route_count(), 1U);
}

TEST(OrderGatewayClientTest, PlaceReadyRouteWritesCommandAndRouteTable) {
  const OrderGatewayShmConfig config = MakeCreateConfig("place_ready");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(MakeReadyEvent(2)));

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;
  ASSERT_EQ(client.PollOrderResponses(runtime), 1U);
  ASSERT_TRUE(client.RouteReady(2));

  const OrderGatewaySendResult sent = client.PlaceOrder(MakeOrder(1001, 2));

  EXPECT_EQ(sent.status, OrderGatewaySendStatus::kOk);
  EXPECT_EQ(client.RouteForLocalOrderForTest(1001), 2U);
  EXPECT_EQ(client.stats().commands_enqueued, 1U);

  OrderGatewayCommand command{};
  ASSERT_TRUE(shm.CommandQueue(2).TryPop(&command));
  EXPECT_EQ(command.kind, OrderGatewayCommandKind::kPlace);
  EXPECT_EQ(command.command_seq, 1U);
  EXPECT_EQ(command.parent_id, 1001U);
  EXPECT_EQ(command.local_order_id, 1001U);
  EXPECT_EQ(command.route_id, 2U);
  EXPECT_EQ(std::string_view(command.symbol, command.symbol_size), "BTC_USDT");
  EXPECT_EQ(std::string_view(command.quantity_text, command.quantity_text_size),
            "0.01");
  EXPECT_EQ(std::string_view(command.price_text, command.price_text_size),
            "65000");
}

TEST(OrderGatewayClientTest, PlaceCommandUsesParentIdWhenProvided) {
  const OrderGatewayShmConfig config = MakeCreateConfig("place_parent");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(0).TryPush(MakeReadyEvent(0)));

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;
  ASSERT_EQ(client.PollOrderResponses(runtime), 1U);

  StrategyOrder order = MakeOrder(1006, 0);
  order.parent_id = 9006;
  ASSERT_EQ(client.PlaceOrder(order).status, OrderGatewaySendStatus::kOk);

  OrderGatewayCommand command{};
  ASSERT_TRUE(shm.CommandQueue(0).TryPop(&command));
  EXPECT_EQ(command.parent_id, 9006U);
  EXPECT_EQ(command.local_order_id, 1006U);
}

TEST(OrderGatewayClientTest, PlaceNotReadyRouteSkipsCommand) {
  const OrderGatewayShmConfig config = MakeCreateConfig("place_not_ready");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;

  OrderGatewayClient client = CreateClient(config);

  const OrderGatewaySendResult sent = client.PlaceOrder(MakeOrder(1002, 3));

  EXPECT_EQ(sent.status, OrderGatewaySendStatus::kRouteNotReady);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1002));
  EXPECT_EQ(client.stats().commands_skipped_route_not_ready, 1U);
  OrderGatewayCommand command{};
  EXPECT_FALSE(shm.CommandQueue(3).TryPop(&command));
}

TEST(OrderGatewayClientTest, HeaderStoppedRouteInvalidatesReadyBeforePlace) {
  const OrderGatewayShmConfig config = MakeCreateConfig("header_stopped");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  StoreOrderGatewayRouteState(shm.header(), 2, OrderGatewayRouteState::kReady);

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;
  EXPECT_EQ(client.PollOrderResponses(runtime), 0U);
  ASSERT_TRUE(client.RouteReady(2));
  StoreOrderGatewayRouteState(shm.header(), 2,
                              OrderGatewayRouteState::kStopped);

  const OrderGatewaySendResult sent = client.PlaceOrder(MakeOrder(1014, 2));

  EXPECT_EQ(sent.status, OrderGatewaySendStatus::kRouteNotReady);
  EXPECT_FALSE(client.RouteReady(2));
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1014));
  OrderGatewayCommand command{};
  EXPECT_FALSE(shm.CommandQueue(2).TryPop(&command));
}

TEST(OrderGatewayClientTest, HeaderStoppedRouteEmitsUnknownForRouteOrders) {
  const OrderGatewayShmConfig config = MakeCreateConfig("stopped_unknown");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  StoreOrderGatewayRouteState(shm.header(), 2, OrderGatewayRouteState::kReady);

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;
  EXPECT_EQ(client.PollOrderResponses(runtime), 0U);
  ASSERT_EQ(client.PlaceOrder(MakeOrder(1016, 2)).status,
            OrderGatewaySendStatus::kOk);
  ASSERT_TRUE(client.HasRouteForLocalOrderForTest(1016));
  StoreOrderGatewayRouteState(shm.header(), 2,
                              OrderGatewayRouteState::kStopped);

  EXPECT_EQ(client.PollOrderResponses(runtime), 1U);

  ASSERT_EQ(runtime.responses.size(), 1U);
  EXPECT_EQ(runtime.responses[0].kind, OrderResponseKind::kUnknownResult);
  EXPECT_EQ(runtime.responses[0].local_order_id, 1016U);
  EXPECT_GT(runtime.responses[0].local_receive_ns, 0);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1016));
  EXPECT_FALSE(client.RouteReady(2));
}

TEST(OrderGatewayClientTest, QueuedFinalResponseBeatsStoppedHeaderUnknown) {
  const OrderGatewayShmConfig config = MakeCreateConfig("final_before_stopped");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  StoreOrderGatewayRouteState(shm.header(), 2, OrderGatewayRouteState::kReady);

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;
  EXPECT_EQ(client.PollOrderResponses(runtime), 0U);
  ASSERT_EQ(client.PlaceOrder(MakeOrder(1017, 2)).status,
            OrderGatewaySendStatus::kOk);
  ASSERT_TRUE(client.HasRouteForLocalOrderForTest(1017));

  OrderGatewayEvent reject{};
  reject.kind = OrderGatewayEventKind::kOrderResponse;
  reject.response_kind = OrderResponseKind::kRejected;
  reject.local_order_id = 1017;
  reject.worker_event_enqueue_ns = 9017;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(reject));
  StoreOrderGatewayRouteState(shm.header(), 2,
                              OrderGatewayRouteState::kStopped);

  EXPECT_EQ(client.PollOrderResponses(runtime), 1U);

  ASSERT_EQ(runtime.responses.size(), 1U);
  EXPECT_EQ(runtime.responses[0].kind, OrderResponseKind::kRejected);
  EXPECT_EQ(runtime.responses[0].local_order_id, 1017U);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1017));
  EXPECT_FALSE(client.RouteReady(2));
}

TEST(OrderGatewayClientTest,
     StoppedRouteDrainsQueuedFinalBeyondNormalPollBudget) {
  const OrderGatewayShmConfig config = MakeCreateConfig("stopped_drain_budget");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  StoreOrderGatewayRouteState(shm.header(), 2, OrderGatewayRouteState::kReady);

  OrderGatewayClient client = CreateClient(
      config, OrderGatewayClientOptions{.max_events_per_poll_per_route = 1});
  CapturingRuntime runtime;
  EXPECT_EQ(client.PollOrderResponses(runtime), 0U);
  ASSERT_EQ(client.PlaceOrder(MakeOrder(1018, 2)).status,
            OrderGatewaySendStatus::kOk);
  ASSERT_TRUE(client.HasRouteForLocalOrderForTest(1018));

  ASSERT_TRUE(shm.EventQueue(2).TryPush(MakeReadyEvent(2)));
  OrderGatewayEvent reject{};
  reject.kind = OrderGatewayEventKind::kOrderResponse;
  reject.response_kind = OrderResponseKind::kRejected;
  reject.local_order_id = 1018;
  reject.worker_event_enqueue_ns = 9018;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(reject));
  StoreOrderGatewayRouteState(shm.header(), 2,
                              OrderGatewayRouteState::kStopped);

  EXPECT_EQ(client.PollOrderResponses(runtime), 2U);

  ASSERT_EQ(runtime.responses.size(), 1U);
  EXPECT_EQ(runtime.responses[0].kind, OrderResponseKind::kRejected);
  EXPECT_EQ(runtime.responses[0].local_order_id, 1018U);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1018));
  EXPECT_FALSE(client.RouteReady(2));
}

TEST(OrderGatewayClientTest,
     StoppedRouteQueuedUnknownBeyondNormalPollBudgetSuppressesSynthetic) {
  const OrderGatewayShmConfig config =
      MakeCreateConfig("stopped_drain_unknown_budget");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  StoreOrderGatewayRouteState(shm.header(), 2, OrderGatewayRouteState::kReady);

  OrderGatewayClient client = CreateClient(
      config, OrderGatewayClientOptions{.max_events_per_poll_per_route = 1});
  CapturingRuntime runtime;
  EXPECT_EQ(client.PollOrderResponses(runtime), 0U);
  ASSERT_EQ(client.PlaceOrder(MakeOrder(1019, 2)).status,
            OrderGatewaySendStatus::kOk);
  ASSERT_TRUE(client.HasRouteForLocalOrderForTest(1019));

  ASSERT_TRUE(shm.EventQueue(2).TryPush(MakeReadyEvent(2)));
  OrderGatewayEvent unknown{};
  unknown.kind = OrderGatewayEventKind::kOrderResponse;
  unknown.response_kind = OrderResponseKind::kUnknownResult;
  unknown.local_order_id = 1019;
  unknown.worker_event_enqueue_ns = 9019;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(unknown));
  StoreOrderGatewayRouteState(shm.header(), 2,
                              OrderGatewayRouteState::kStopped);

  EXPECT_EQ(client.PollOrderResponses(runtime), 2U);

  ASSERT_EQ(runtime.responses.size(), 1U);
  EXPECT_EQ(runtime.responses[0].kind, OrderResponseKind::kUnknownResult);
  EXPECT_EQ(runtime.responses[0].local_order_id, 1019U);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1019));
  EXPECT_FALSE(client.RouteReady(2));
}

TEST(OrderGatewayClientTest,
     StoppedRouteQueuedAcceptedBeyondNormalPollBudgetStillSynthesizesUnknown) {
  const OrderGatewayShmConfig config =
      MakeCreateConfig("stopped_drain_accepted_budget");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  StoreOrderGatewayRouteState(shm.header(), 2, OrderGatewayRouteState::kReady);

  OrderGatewayClient client = CreateClient(
      config, OrderGatewayClientOptions{.max_events_per_poll_per_route = 1});
  CapturingRuntime runtime;
  EXPECT_EQ(client.PollOrderResponses(runtime), 0U);
  ASSERT_EQ(client.PlaceOrder(MakeOrder(1020, 2)).status,
            OrderGatewaySendStatus::kOk);
  ASSERT_TRUE(client.HasRouteForLocalOrderForTest(1020));

  ASSERT_TRUE(shm.EventQueue(2).TryPush(MakeReadyEvent(2)));
  OrderGatewayEvent accepted{};
  accepted.kind = OrderGatewayEventKind::kOrderResponse;
  accepted.response_kind = OrderResponseKind::kAccepted;
  accepted.local_order_id = 1020;
  accepted.worker_event_enqueue_ns = 9020;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(accepted));
  StoreOrderGatewayRouteState(shm.header(), 2,
                              OrderGatewayRouteState::kStopped);

  EXPECT_EQ(client.PollOrderResponses(runtime), 3U);

  ASSERT_EQ(runtime.responses.size(), 2U);
  EXPECT_EQ(runtime.responses[0].kind, OrderResponseKind::kAccepted);
  EXPECT_EQ(runtime.responses[0].local_order_id, 1020U);
  EXPECT_EQ(runtime.responses[1].kind, OrderResponseKind::kUnknownResult);
  EXPECT_EQ(runtime.responses[1].local_order_id, 1020U);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1020));
  EXPECT_FALSE(client.RouteReady(2));
}

TEST(OrderGatewayClientTest, PollDoesNotLeaveStaleReadyOverStoppedHeader) {
  const OrderGatewayShmConfig config = MakeCreateConfig("poll_stale_ready");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(MakeReadyEvent(2)));
  StoreOrderGatewayRouteState(shm.header(), 2, OrderGatewayRouteState::kStopped);

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;

  EXPECT_EQ(client.PollOrderResponses(runtime), 1U);
  EXPECT_FALSE(client.RouteReady(2));
}

TEST(OrderGatewayClientTest, HeaderReadyAfterAllStoppedRestoresRunning) {
  const OrderGatewayShmConfig config = MakeCreateConfig("header_restarted");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  for (std::uint16_t route = 0; route < config.route_count; ++route) {
    StoreOrderGatewayRouteState(shm.header(), route,
                                OrderGatewayRouteState::kStopped);
  }

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;
  EXPECT_EQ(client.PollOrderResponses(runtime), 0U);
  EXPECT_FALSE(client.Running());

  StoreOrderGatewayRouteState(shm.header(), 0, OrderGatewayRouteState::kReady);
  EXPECT_EQ(client.PollOrderResponses(runtime), 0U);

  EXPECT_TRUE(client.Running());
  ASSERT_TRUE(client.RouteReady(0));
  EXPECT_EQ(client.PlaceOrder(MakeOrder(1015, 0)).status,
            OrderGatewaySendStatus::kOk);
  OrderGatewayCommand command{};
  EXPECT_TRUE(shm.CommandQueue(0).TryPop(&command));
}

TEST(OrderGatewayClientTest, AutoRouteWithNoReadyRouteReportsNotReady) {
  const OrderGatewayShmConfig config = MakeCreateConfig("auto_no_ready");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;

  OrderGatewayClient client = CreateClient(config);

  const OrderGatewaySendResult sent =
      client.PlaceOrder(MakeOrder(1007, kAutoGatewayRoute));

  EXPECT_EQ(sent.status, OrderGatewaySendStatus::kRouteNotReady);
  EXPECT_EQ(client.stats().invalid_routes, 0U);
  EXPECT_EQ(client.stats().commands_skipped_route_not_ready, 1U);
  OrderGatewayCommand command{};
  for (std::uint16_t route = 0; route < config.route_count; ++route) {
    EXPECT_FALSE(shm.CommandQueue(route).TryPop(&command));
  }
}

TEST(OrderGatewayClientTest, FullCommandQueueReturnsSessionRejected) {
  OrderGatewayShmConfig config = MakeCreateConfig("queue_full");
  config.command_queue_capacity = 1;
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(0).TryPush(MakeReadyEvent(0)));

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;
  ASSERT_EQ(client.PollOrderResponses(runtime), 1U);
  ASSERT_EQ(client.PlaceOrder(MakeOrder(1003, 0)).status,
            OrderGatewaySendStatus::kOk);

  const OrderGatewaySendResult rejected = client.PlaceOrder(MakeOrder(1004, 0));

  EXPECT_EQ(rejected.status, OrderGatewaySendStatus::kCommandQueueFull);
  EXPECT_EQ(client.stats().command_enqueue_failures, 1U);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1004));
}

TEST(OrderGatewayClientTest, RouteTableCapacityRejectsBeforeEnqueue) {
  const OrderGatewayShmConfig config = MakeCreateConfig("route_table_full");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(0).TryPush(MakeReadyEvent(0)));

  OrderGatewayClient client = CreateClient(
      config, OrderGatewayClientOptions{.route_table_capacity = 1});
  CapturingRuntime runtime;
  ASSERT_EQ(client.PollOrderResponses(runtime), 1U);

  ASSERT_EQ(client.PlaceOrder(MakeOrder(1008, 0)).status,
            OrderGatewaySendStatus::kOk);
  const OrderGatewaySendResult rejected = client.PlaceOrder(MakeOrder(1009, 0));

  EXPECT_EQ(rejected.status, OrderGatewaySendStatus::kRouteTableFull);
  EXPECT_EQ(client.stats().route_table_full_rejections, 1U);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1009));

  OrderGatewayCommand command{};
  ASSERT_TRUE(shm.CommandQueue(0).TryPop(&command));
  EXPECT_EQ(command.local_order_id, 1008U);
  EXPECT_FALSE(shm.CommandQueue(0).TryPop(&command));
}

TEST(OrderGatewayClientTest, CommandRejectedEventConvertsToOrderResponse) {
  const OrderGatewayShmConfig config = MakeCreateConfig("command_rejected");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;

  OrderGatewayEvent event{};
  event.kind = OrderGatewayEventKind::kCommandRejected;
  event.response_kind = OrderResponseKind::kAck;
  event.local_order_id = 1005;
  event.exchange_order_id = 7005;
  event.worker_event_enqueue_ns = 9005;
  ASSERT_TRUE(shm.EventQueue(1).TryPush(event));

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;

  EXPECT_EQ(client.PollOrderResponses(runtime), 1U);
  ASSERT_EQ(runtime.responses.size(), 1U);
  EXPECT_EQ(runtime.responses[0].kind, OrderResponseKind::kRejected);
  EXPECT_EQ(runtime.responses[0].local_order_id, 1005U);
  EXPECT_EQ(runtime.responses[0].exchange_order_id, 7005U);
  EXPECT_EQ(runtime.responses[0].local_receive_ns, 9005);
  EXPECT_EQ(client.stats().command_rejected_events, 1U);
}

TEST(OrderGatewayClientTest, PlaceCommandRejectedClearsRouteTable) {
  const OrderGatewayShmConfig config = MakeCreateConfig("place_reject_cleanup");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(1).TryPush(MakeReadyEvent(1)));

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;
  ASSERT_EQ(client.PollOrderResponses(runtime), 1U);
  ASSERT_EQ(client.PlaceOrder(MakeOrder(1012, 1)).status,
            OrderGatewaySendStatus::kOk);
  ASSERT_TRUE(client.HasRouteForLocalOrderForTest(1012));

  OrderGatewayCommand command{};
  ASSERT_TRUE(shm.CommandQueue(1).TryPop(&command));

  OrderGatewayEvent reject{};
  reject.kind = OrderGatewayEventKind::kCommandRejected;
  reject.command_kind = OrderGatewayCommandKind::kPlace;
  reject.local_order_id = 1012;
  reject.worker_event_enqueue_ns = 9012;
  ASSERT_TRUE(shm.EventQueue(1).TryPush(reject));

  EXPECT_EQ(client.PollOrderResponses(runtime), 1U);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1012));
  ASSERT_EQ(runtime.responses.size(), 1U);
  EXPECT_EQ(runtime.responses[0].kind, OrderResponseKind::kRejected);
}

TEST(OrderGatewayClientTest,
     CancelCommandRejectedKeepsRouteAndUnblocksManager) {
  const OrderGatewayShmConfig config = MakeCreateConfig("cancel_reject");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(MakeReadyEvent(2)));

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime ready_runtime;
  ASSERT_EQ(client.PollOrderResponses(ready_runtime), 1U);
  OrderManager<OrderGatewayClient> manager(client, 8, 7);
  OrderCreateRequest request{};
  request.symbol_id = 42;
  request.symbol = "BTC_USDT";
  request.quantity = 0.01;
  request.quantity_text = "0.01";
  request.price_text = "65000";
  request.gateway_route_id = 2;
  const OrderPlaceResult placed = manager.PlaceOrder(request);
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);

  OrderGatewayCommand command{};
  ASSERT_TRUE(shm.CommandQueue(2).TryPop(&command));
  ASSERT_EQ(command.kind, OrderGatewayCommandKind::kPlace);
  ASSERT_EQ(manager.CancelOrder(placed.local_order_id).status,
            OrderCancelStatus::kOk);
  ASSERT_TRUE(shm.CommandQueue(2).TryPop(&command));
  ASSERT_EQ(command.kind, OrderGatewayCommandKind::kCancel);

  OrderGatewayEvent reject{};
  reject.kind = OrderGatewayEventKind::kCommandRejected;
  reject.command_kind = OrderGatewayCommandKind::kCancel;
  reject.local_order_id = placed.local_order_id;
  reject.worker_event_enqueue_ns = 9013;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(reject));

  OrderManagerRuntime runtime(manager);
  EXPECT_EQ(client.PollOrderResponses(runtime), 1U);
  ASSERT_EQ(runtime.responses.size(), 1U);
  EXPECT_EQ(runtime.responses[0].kind, OrderResponseKind::kCancelRejected);
  const StrategyOrder* order = manager.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_NE(order->status, OrderStatus::kCancelSent);
  EXPECT_TRUE(client.HasRouteForLocalOrderForTest(placed.local_order_id));
}

TEST(OrderGatewayClientTest, RejectedOrderResponseClearsRouteTable) {
  const OrderGatewayShmConfig config = MakeCreateConfig("reject_cleanup");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(MakeReadyEvent(2)));

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;
  ASSERT_EQ(client.PollOrderResponses(runtime), 1U);
  ASSERT_EQ(client.PlaceOrder(MakeOrder(1010, 2)).status,
            OrderGatewaySendStatus::kOk);
  ASSERT_TRUE(client.HasRouteForLocalOrderForTest(1010));

  OrderGatewayEvent reject{};
  reject.kind = OrderGatewayEventKind::kOrderResponse;
  reject.response_kind = OrderResponseKind::kRejected;
  reject.local_order_id = 1010;
  reject.worker_event_enqueue_ns = 9010;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(reject));

  EXPECT_EQ(client.PollOrderResponses(runtime), 1U);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1010));
  ASSERT_EQ(runtime.responses.size(), 1U);
  EXPECT_EQ(runtime.responses[0].kind, OrderResponseKind::kRejected);
}

TEST(OrderGatewayClientTest, CancelCacheAndForgetUseOriginalRoute) {
  const OrderGatewayShmConfig config = MakeCreateConfig("cancel_cache_forget");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(2).TryPush(MakeReadyEvent(2)));

  OrderGatewayClient client = CreateClient(config);
  CapturingRuntime runtime;
  ASSERT_EQ(client.PollOrderResponses(runtime), 1U);
  StrategyOrder order = MakeOrder(1011, 2);
  ASSERT_EQ(client.PlaceOrder(order).status, OrderGatewaySendStatus::kOk);

  OrderGatewayCommand command{};
  ASSERT_TRUE(shm.CommandQueue(2).TryPop(&command));
  ASSERT_EQ(command.kind, OrderGatewayCommandKind::kPlace);

  order.gateway_route_id = 0;
  order.exchange_order_id = 7011;
  ASSERT_EQ(client.CancelOrder(order).status, OrderGatewaySendStatus::kOk);
  client.CacheExchangeOrderId(order.local_order_id, order.exchange_order_id);
  client.ForgetExchangeOrderId(order.local_order_id);

  ASSERT_TRUE(shm.CommandQueue(2).TryPop(&command));
  EXPECT_EQ(command.kind, OrderGatewayCommandKind::kCancel);
  EXPECT_EQ(command.local_order_id, 1011U);
  EXPECT_EQ(command.exchange_order_id, 7011U);
  EXPECT_EQ(command.route_id, 2U);

  ASSERT_TRUE(shm.CommandQueue(2).TryPop(&command));
  EXPECT_EQ(command.kind, OrderGatewayCommandKind::kCacheExchangeOrderId);
  EXPECT_EQ(command.exchange_order_id, 7011U);
  EXPECT_EQ(command.route_id, 2U);

  ASSERT_TRUE(shm.CommandQueue(2).TryPop(&command));
  EXPECT_EQ(command.kind, OrderGatewayCommandKind::kForgetExchangeOrderId);
  EXPECT_EQ(command.route_id, 2U);
  EXPECT_FALSE(client.HasRouteForLocalOrderForTest(1011));
}

TEST(OrderGatewayClientTest, RejectsZeroEventDrainBudget) {
  const OrderGatewayShmConfig config = MakeCreateConfig("zero_poll_budget");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;

  auto client_result = OrderGatewayClient::Open(MakeClientConfig(
      config, OrderGatewayClientOptions{.max_events_per_poll_per_route = 0}));

  EXPECT_FALSE(client_result.ok);
}

TEST(OrderGatewayClientTest, RejectsZeroRouteTableCapacity) {
  const OrderGatewayShmConfig config = MakeCreateConfig("zero_route_table");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;

  auto client_result = OrderGatewayClient::Open(MakeClientConfig(
      config, OrderGatewayClientOptions{.route_table_capacity = 0}));

  EXPECT_FALSE(client_result.ok);
}

}  // namespace
}  // namespace aquila::core
