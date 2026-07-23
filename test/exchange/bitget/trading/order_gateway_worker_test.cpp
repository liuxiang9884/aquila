#include "exchange/bitget/trading/order_gateway_worker.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/trading/order_gateway_shm.h"

namespace aquila::bitget {
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
  return fmt::format("/aquila_bitget_order_gateway_worker_test_{}_{}",
                     ::getpid(), suffix);
}

core::OrderGatewayShmConfig MakeShmConfig(std::string_view suffix) {
  return core::OrderGatewayShmConfig{
      .shm_name = UniqueShmName(suffix),
      .create = true,
      .remove_existing = true,
      .route_count = 2,
      .command_queue_capacity = 8,
      .event_queue_capacity = 16,
      .startup_ready_timeout_s = 30,
  };
}

class FakeSession {
 public:
  OrderSendResult PlaceOrder(const core::OrderPlaceRequest& order) noexcept {
    placed.push_back(order.local_order_id);
    last_exchange = order.exchange;
    last_symbol.assign(order.SymbolView());
    return place_result;
  }

  OrderSendResult CancelOrder(const core::OrderCancelRequest& order) noexcept {
    cancelled.push_back(order.local_order_id);
    return cancel_result;
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    cached_local_ids.push_back(local_order_id);
    cached_exchange_ids.push_back(exchange_order_id);
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    forgotten_local_ids.push_back(local_order_id);
  }

  OrderSendResult place_result{.status = OrderSendStatus::kOk,
                               .request_sequence = 101,
                               .encoded_request_id = 201,
                               .send_local_ns = 301};
  OrderSendResult cancel_result{.status = OrderSendStatus::kOk,
                                .request_sequence = 102,
                                .encoded_request_id = 202,
                                .send_local_ns = 302};
  Exchange last_exchange{Exchange::kGate};
  std::string last_symbol;
  std::vector<std::uint64_t> placed;
  std::vector<std::uint64_t> cancelled;
  std::vector<std::uint64_t> cached_local_ids;
  std::vector<std::uint64_t> cached_exchange_ids;
  std::vector<std::uint64_t> forgotten_local_ids;
};

core::OrderGatewayCommand MakePlaceCommand(std::uint16_t route_id,
                                           std::uint64_t local_order_id) {
  core::OrderGatewayCommand command{};
  command.kind = core::OrderGatewayCommandKind::kPlace;
  command.command_seq = 10 + local_order_id;
  command.payload.place = core::OrderPlaceRequest{
      .local_order_id = local_order_id,
      .group_id = 901,
      .price = 65000.0,
      .quantity = 0.01,
      .symbol_id = 42,
      .gateway_route_id = route_id,
      .exchange = Exchange::kBitget,
      .side = OrderSide::kBuy,
      .order_type = OrderType::kLimit,
      .time_in_force = TimeInForce::kImmediateOrCancel,
      .price_decimal_places = 0,
      .quantity_decimal_places = 2,
  };
  core::SetOrderSymbol(&command.payload.place, "BTCUSDT");
  return command;
}

core::OrderGatewayCommand MakeCancelCommand(
    std::uint16_t route_id, std::uint64_t local_order_id,
    [[maybe_unused]] std::uint64_t exchange_order_id) {
  core::OrderGatewayCommand command{};
  command.kind = core::OrderGatewayCommandKind::kCancel;
  command.command_seq = 10 + local_order_id;
  command.payload.cancel = core::OrderCancelRequest{
      .local_order_id = local_order_id,
      .group_id = 901,
      .gateway_route_id = route_id,
  };
  return command;
}

TEST(OrderGatewayWorkerTest, PlaceCommandCallsBitgetSession) {
  const auto config = MakeShmConfig("place_ok");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  auto& shm = shm_result.value;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakePlaceCommand(0, 1001)));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);

  EXPECT_TRUE(worker.PollOnce());
  EXPECT_EQ(session.placed, std::vector<std::uint64_t>({1001}));
  EXPECT_EQ(session.last_exchange, Exchange::kBitget);
  EXPECT_EQ(session.last_symbol, "BTCUSDT");
  core::OrderGatewayEvent event{};
  EXPECT_FALSE(shm.EventQueue(0).TryPop(&event));
}

TEST(OrderGatewayWorkerTest, WrongRouteEmitsCommandRejected) {
  const auto config = MakeShmConfig("wrong_route");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  auto& shm = shm_result.value;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakePlaceCommand(1, 1002)));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);
  EXPECT_TRUE(worker.PollOnce());

  core::OrderGatewayEvent event{};
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.kind, core::OrderGatewayEventKind::kCommandRejected);
  EXPECT_EQ(event.reject_reason,
            core::OrderGatewayCommandRejectReason::kInvalidCommand);
  EXPECT_EQ(event.local_order_id, 1002U);
  EXPECT_GT(event.worker_dequeue_ns, 0);
}

TEST(OrderGatewayWorkerTest, BitgetSendStatusesMapToCoreReasons) {
  using Reason = core::OrderGatewayCommandRejectReason;
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kNotLoggedIn),
            Reason::kSessionNotReady);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kNotActive),
            Reason::kSessionNotActive);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kInflightFull),
            Reason::kInflightFull);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kOrderIdCacheFull),
            Reason::kInflightFull);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kInvalidRoute),
            Reason::kInvalidCommand);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kInvalidSymbol),
            Reason::kEncodeFailed);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kInvalidPriceText),
            Reason::kEncodeFailed);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kUnsupportedOrderType),
            Reason::kUnsupportedOrderType);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kNoPreparedWriteSlot),
            Reason::kNoPreparedWriteSlot);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kWriteUnavailable),
            Reason::kWriteUnavailable);
}

TEST(OrderGatewayWorkerTest, SendFailureEmitsCommandRejected) {
  const auto config = MakeShmConfig("send_failure");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  auto& shm = shm_result.value;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakePlaceCommand(0, 1003)));

  FakeSession session;
  session.place_result = {.status = OrderSendStatus::kWriteUnavailable,
                          .request_sequence = 501,
                          .encoded_request_id = 601,
                          .send_local_ns = 0};
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);
  EXPECT_TRUE(worker.PollOnce());

  core::OrderGatewayEvent event{};
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.reject_reason,
            core::OrderGatewayCommandRejectReason::kWriteUnavailable);
  EXPECT_EQ(event.request_sequence, 501U);
  EXPECT_EQ(event.encoded_request_id, 601U);
}

TEST(OrderGatewayWorkerTest, AckConsumesRequestMetadata) {
  const auto config = MakeShmConfig("ack_metadata");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  auto& shm = shm_result.value;
  auto command = MakePlaceCommand(0, 1104);
  command.command_seq = 44;
  command.payload.place.group_id = 43;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(command));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);
  ASSERT_TRUE(worker.PollOnce());

  const OrderResponse ack{.kind = OrderResponseKind::kAck,
                          .local_order_id = 1104,
                          .request_sequence = 101,
                          .local_receive_ns = 900,
                          .exchange_ns = 800};
  publisher.OnOrderResponse(ack);
  publisher.OnOrderResponse(ack);

  core::OrderGatewayEvent event{};
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.command_seq, 44U);
  EXPECT_EQ(event.group_id, 43U);
  EXPECT_EQ(event.request_send_local_ns, 301);
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.command_seq, 0U);
  EXPECT_EQ(event.group_id, 0U);
  EXPECT_EQ(event.request_send_local_ns, 0);
}

TEST(OrderGatewayWorkerTest, OutOfOrderResponsesKeepPerGroupCommandMetadata) {
  const auto config = MakeShmConfig("out_of_order_group_metadata");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  auto& shm = shm_result.value;

  core::OrderGatewayCommand group_a = MakePlaceCommand(0, 1201);
  group_a.command_seq = 41;
  group_a.payload.place.group_id = 701;
  core::OrderGatewayCommand group_b = MakePlaceCommand(0, 1202);
  group_b.command_seq = 42;
  group_b.payload.place.group_id = 702;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(group_a));
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(group_b));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);
  session.place_result.request_sequence = 101;
  ASSERT_TRUE(worker.PollOnce());
  session.place_result.request_sequence = 102;
  ASSERT_TRUE(worker.PollOnce());

  publisher.OnOrderResponse(OrderResponse{
      .kind = OrderResponseKind::kRejected,
      .local_order_id = 1202,
      .request_sequence = 102,
  });
  publisher.OnOrderResponse(OrderResponse{
      .kind = OrderResponseKind::kRejected,
      .local_order_id = 1201,
      .request_sequence = 101,
  });

  core::OrderGatewayEvent event{};
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.local_order_id, 1202U);
  EXPECT_EQ(event.command_seq, 42U);
  EXPECT_EQ(event.group_id, 702U);
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.local_order_id, 1201U);
  EXPECT_EQ(event.command_seq, 41U);
  EXPECT_EQ(event.group_id, 701U);
}

TEST(OrderGatewayWorkerTest, NotReadyClearsPendingMetadata) {
  const auto config = MakeShmConfig("not_ready_metadata");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  auto& shm = shm_result.value;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakePlaceCommand(0, 1105)));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0), &shm.header());
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);
  ASSERT_TRUE(worker.PollOnce());
  publisher.OnOrderSessionLoginNotReady();
  publisher.OnOrderResponse(OrderResponse{.kind = OrderResponseKind::kRejected,
                                          .local_order_id = 1105,
                                          .request_sequence = 101});

  core::OrderGatewayEvent event{};
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  ASSERT_EQ(event.kind, core::OrderGatewayEventKind::kNotReady);
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.command_seq, 0U);
  EXPECT_EQ(event.group_id, 0U);
}

TEST(OrderGatewayWorkerTest, CancelCacheAndForgetDispatchToSession) {
  const auto config = MakeShmConfig("cancel_cache");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  auto& shm = shm_result.value;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakeCancelCommand(0, 1004, 7004)));
  core::OrderGatewayCommand cache{};
  cache.kind = core::OrderGatewayCommandKind::kCacheExchangeOrderId;
  cache.payload.order_id = core::OrderGatewayOrderIdCommand{
      .local_order_id = 1004,
      .exchange_order_id = 7004,
      .gateway_route_id = 0,
  };
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(cache));
  core::OrderGatewayCommand forget{};
  forget.kind = core::OrderGatewayCommandKind::kForgetExchangeOrderId;
  forget.payload.order_id = core::OrderGatewayOrderIdCommand{
      .local_order_id = 1004,
      .gateway_route_id = 0,
  };
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(forget));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);
  EXPECT_EQ(worker.Drain(3), 3U);
  EXPECT_EQ(session.cancelled, std::vector<std::uint64_t>({1004}));
  EXPECT_EQ(session.cached_exchange_ids, std::vector<std::uint64_t>({7004}));
  EXPECT_EQ(session.forgotten_local_ids, std::vector<std::uint64_t>({1004}));
}

TEST(OrderGatewayWorkerTest, StopCommandDoesNotRequireRouteId) {
  const auto config = MakeShmConfig("stop");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  auto& shm = shm_result.value;
  core::OrderGatewayCommand stop{};
  stop.kind = core::OrderGatewayCommandKind::kStop;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(stop));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);
  EXPECT_TRUE(worker.PollOnce());
  EXPECT_TRUE(worker.stopped());
  core::OrderGatewayEvent event{};
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.kind, core::OrderGatewayEventKind::kStopped);
}

TEST(OrderGatewayWorkerTest, ReadyCallbacksUpdateRouteState) {
  const auto config = MakeShmConfig("ready");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  auto& shm = shm_result.value;

  OrderGatewayWorkerPublisher publisher(1, shm.EventQueue(1), &shm.header());
  publisher.OnOrderSessionLoginReady();
  EXPECT_EQ(core::LoadOrderGatewayRouteState(shm.header(), 1),
            core::OrderGatewayRouteState::kReady);
  publisher.OnOrderSessionLoginNotReady();
  EXPECT_EQ(core::LoadOrderGatewayRouteState(shm.header(), 1),
            core::OrderGatewayRouteState::kNotReady);
}

TEST(OrderGatewayWorkerTest, EventQueueFailureStopsRoute) {
  auto config = MakeShmConfig("event_full");
  config.event_queue_capacity = 1;
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  auto& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(0).TryPush(core::OrderGatewayEvent{}));
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakePlaceCommand(1, 1201)));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0), &shm.header());
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);
  EXPECT_TRUE(worker.PollOnce());
  EXPECT_TRUE(worker.stopped());
  EXPECT_EQ(core::LoadOrderGatewayRouteState(shm.header(), 0),
            core::OrderGatewayRouteState::kStopped);
}

}  // namespace
}  // namespace aquila::bitget
