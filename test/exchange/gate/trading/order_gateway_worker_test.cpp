#include "exchange/gate/trading/order_gateway_worker.h"

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

namespace aquila::gate {
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
  return fmt::format("/aquila_gate_order_gateway_worker_test_{}_{}", ::getpid(),
                     suffix);
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
  [[nodiscard]] bool Ready() const noexcept {
    return ready;
  }

  OrderSendResult PlaceOrder(const core::StrategyOrder& order) noexcept {
    placed.push_back(order.local_order_id);
    last_symbol.assign(order.symbol.data(), order.symbol.size());
    return place_result;
  }

  OrderSendResult CancelOrder(const core::StrategyOrder& order) noexcept {
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

  bool ready{true};
  OrderSendResult place_result{.status = OrderSendStatus::kOk,
                               .request_sequence = 101,
                               .encoded_request_id = 201,
                               .send_local_ns = 301};
  OrderSendResult cancel_result{.status = OrderSendStatus::kOk,
                                .request_sequence = 102,
                                .encoded_request_id = 202,
                                .send_local_ns = 302};
  std::vector<std::uint64_t> placed;
  std::vector<std::uint64_t> cancelled;
  std::vector<std::uint64_t> cached_local_ids;
  std::vector<std::uint64_t> cached_exchange_ids;
  std::vector<std::uint64_t> forgotten_local_ids;
  std::string last_symbol;
};

core::OrderGatewayCommand MakePlaceCommand(std::uint16_t route_id,
                                           std::uint64_t local_order_id) {
  core::OrderGatewayCommand command{};
  command.kind = core::OrderGatewayCommandKind::kPlace;
  command.command_seq = 10 + local_order_id;
  command.parent_id = 9000;
  command.local_order_id = local_order_id;
  command.route_id = route_id;
  command.exchange = Exchange::kGate;
  command.symbol_id = 42;
  command.side = OrderSide::kBuy;
  command.order_type = OrderType::kLimit;
  command.time_in_force = TimeInForce::kImmediateOrCancel;
  command.quantity = 0.01;
  command.reduce_only = 0;
  const std::string_view symbol = "BTC_USDT";
  const std::string_view quantity = "0.01";
  const std::string_view price = "65000";
  command.symbol_size = static_cast<std::uint16_t>(symbol.size());
  command.quantity_text_size = static_cast<std::uint16_t>(quantity.size());
  command.price_text_size = static_cast<std::uint16_t>(price.size());
  std::memcpy(command.symbol, symbol.data(), symbol.size());
  std::memcpy(command.quantity_text, quantity.data(), quantity.size());
  std::memcpy(command.price_text, price.data(), price.size());
  return command;
}

core::OrderGatewayCommand MakeCancelCommand(std::uint16_t route_id,
                                            std::uint64_t local_order_id,
                                            std::uint64_t exchange_order_id) {
  core::OrderGatewayCommand command =
      MakePlaceCommand(route_id, local_order_id);
  command.kind = core::OrderGatewayCommandKind::kCancel;
  command.exchange_order_id = exchange_order_id;
  return command;
}

TEST(OrderGatewayWorkerTest, PlaceCommandCallsSessionWithoutImmediateEvent) {
  const core::OrderGatewayShmConfig config = MakeShmConfig("place_ok");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  core::OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakePlaceCommand(0, 1001)));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);

  EXPECT_TRUE(worker.PollOnce());
  EXPECT_EQ(session.placed, std::vector<std::uint64_t>({1001}));
  EXPECT_EQ(session.last_symbol, "BTC_USDT");
  core::OrderGatewayEvent event{};
  EXPECT_FALSE(shm.EventQueue(0).TryPop(&event));
}

TEST(OrderGatewayWorkerTest, WrongRouteEmitsCommandRejected) {
  const core::OrderGatewayShmConfig config = MakeShmConfig("wrong_route");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  core::OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakePlaceCommand(1, 1002)));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);

  EXPECT_TRUE(worker.PollOnce());
  EXPECT_TRUE(session.placed.empty());
  core::OrderGatewayEvent event{};
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.kind, core::OrderGatewayEventKind::kCommandRejected);
  EXPECT_EQ(event.command_kind, core::OrderGatewayCommandKind::kPlace);
  EXPECT_EQ(event.response_kind, core::OrderResponseKind::kRejected);
  EXPECT_EQ(event.reject_reason,
            core::OrderGatewayCommandRejectReason::kInvalidCommand);
  EXPECT_EQ(event.local_order_id, 1002U);
  EXPECT_EQ(event.route_id, 0U);
  EXPECT_GT(event.worker_dequeue_ns, 0);
  EXPECT_GT(event.worker_event_enqueue_ns, 0);
}

TEST(OrderGatewayWorkerTest, SendFailureEmitsCommandRejected) {
  const core::OrderGatewayShmConfig config = MakeShmConfig("send_failure");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  core::OrderGatewayShmManager& shm = shm_result.value;
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
  EXPECT_EQ(event.kind, core::OrderGatewayEventKind::kCommandRejected);
  EXPECT_EQ(event.reject_reason,
            core::OrderGatewayCommandRejectReason::kWriteUnavailable);
  EXPECT_EQ(event.request_sequence, 501U);
  EXPECT_EQ(event.encoded_request_id, 601U);
  EXPECT_EQ(event.request_send_local_ns, 0);
  EXPECT_GT(event.worker_dequeue_ns, 0);
  EXPECT_GT(event.worker_event_enqueue_ns, 0);
}

TEST(OrderGatewayWorkerTest, EventQueueFullStopsWorkerBeforeNextDispatch) {
  core::OrderGatewayShmConfig config = MakeShmConfig("event_full");
  config.event_queue_capacity = 1;
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  core::OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.EventQueue(0).TryPush(core::OrderGatewayEvent{}));
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakePlaceCommand(1, 1101)));
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakePlaceCommand(0, 1102)));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);

  EXPECT_TRUE(worker.PollOnce());
  EXPECT_TRUE(worker.stopped());
  EXPECT_TRUE(publisher.event_queue_failed());
  EXPECT_FALSE(worker.PollOnce());
  EXPECT_TRUE(session.placed.empty());
}

TEST(OrderGatewayWorkerTest, StopCommandDoesNotRequireRouteId) {
  const core::OrderGatewayShmConfig config = MakeShmConfig("stop_any_route");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  core::OrderGatewayShmManager& shm = shm_result.value;

  core::OrderGatewayCommand stop{};
  stop.kind = core::OrderGatewayCommandKind::kStop;
  stop.command_seq = 99;
  stop.route_id = std::numeric_limits<std::uint16_t>::max();
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

template <typename Mutator>
void ExpectOversizedTextFieldRejected(std::string_view suffix,
                                      Mutator mutator) {
  const core::OrderGatewayShmConfig config = MakeShmConfig(suffix);
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  core::OrderGatewayShmManager& shm = shm_result.value;

  core::OrderGatewayCommand command = MakePlaceCommand(0, 1103);
  mutator(command);
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(command));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);

  EXPECT_TRUE(worker.PollOnce());
  EXPECT_TRUE(session.placed.empty());
  core::OrderGatewayEvent event{};
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.kind, core::OrderGatewayEventKind::kCommandRejected);
  EXPECT_EQ(event.reject_reason,
            core::OrderGatewayCommandRejectReason::kInvalidCommand);
}

TEST(OrderGatewayWorkerTest, OversizedTextFieldsAreRejectedBeforeSessionCall) {
  ExpectOversizedTextFieldRejected(
      "oversized_symbol", [](core::OrderGatewayCommand& command) {
        command.symbol_size = core::kOrderGatewaySymbolBytes + 1;
      });
  ExpectOversizedTextFieldRejected(
      "oversized_quantity", [](core::OrderGatewayCommand& command) {
        command.quantity_text_size = core::kOrderGatewayQuantityTextBytes + 1;
      });
  ExpectOversizedTextFieldRejected(
      "oversized_price", [](core::OrderGatewayCommand& command) {
        command.price_text_size = core::kOrderGatewayPriceTextBytes + 1;
      });
}

TEST(OrderGatewayWorkerTest, ResponseEventCarriesCommandMetadata) {
  const core::OrderGatewayShmConfig config = MakeShmConfig("response_metadata");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  core::OrderGatewayShmManager& shm = shm_result.value;
  core::OrderGatewayCommand command = MakePlaceCommand(0, 1104);
  command.command_seq = 44;
  command.parent_id = 33;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(command));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);
  ASSERT_TRUE(worker.PollOnce());

  publisher.OnOrderResponse(OrderResponse{.kind = OrderResponseKind::kAck,
                                          .local_order_id = 1104,
                                          .request_sequence = 101,
                                          .local_receive_ns = 900,
                                          .exchange_ns = 800});

  core::OrderGatewayEvent event{};
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.kind, core::OrderGatewayEventKind::kOrderResponse);
  EXPECT_EQ(event.command_seq, 44U);
  EXPECT_EQ(event.parent_id, 33U);
  EXPECT_GT(event.worker_dequeue_ns, 0);
  EXPECT_EQ(event.request_send_local_ns, 301);
}

TEST(OrderGatewayWorkerTest, CancelCacheAndForgetDispatchToSession) {
  const core::OrderGatewayShmConfig config = MakeShmConfig("cancel_cache");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  core::OrderGatewayShmManager& shm = shm_result.value;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(MakeCancelCommand(0, 1004, 7004)));

  core::OrderGatewayCommand cache{};
  cache.kind = core::OrderGatewayCommandKind::kCacheExchangeOrderId;
  cache.command_seq = 77;
  cache.local_order_id = 1004;
  cache.exchange_order_id = 7004;
  cache.route_id = 0;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(cache));

  core::OrderGatewayCommand forget{};
  forget.kind = core::OrderGatewayCommandKind::kForgetExchangeOrderId;
  forget.command_seq = 78;
  forget.local_order_id = 1004;
  forget.route_id = 0;
  ASSERT_TRUE(shm.CommandQueue(0).TryPush(forget));

  FakeSession session;
  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<FakeSession> worker(0, shm.CommandQueue(0), session,
                                                publisher);

  EXPECT_TRUE(worker.PollOnce());
  EXPECT_TRUE(worker.PollOnce());
  EXPECT_TRUE(worker.PollOnce());
  EXPECT_EQ(session.cancelled, std::vector<std::uint64_t>({1004}));
  EXPECT_EQ(session.cached_local_ids, std::vector<std::uint64_t>({1004}));
  EXPECT_EQ(session.cached_exchange_ids, std::vector<std::uint64_t>({7004}));
  EXPECT_EQ(session.forgotten_local_ids, std::vector<std::uint64_t>({1004}));
}

TEST(OrderGatewayWorkerTest, ReadyAndNotReadyCallbacksEmitEvents) {
  const core::OrderGatewayShmConfig config = MakeShmConfig("ready_events");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  core::OrderGatewayShmManager& shm = shm_result.value;

  OrderGatewayWorkerPublisher publisher(1, shm.EventQueue(1));
  publisher.OnOrderSessionLoginReady();
  publisher.OnOrderSessionLoginNotReady();

  core::OrderGatewayEvent ready{};
  ASSERT_TRUE(shm.EventQueue(1).TryPop(&ready));
  EXPECT_EQ(ready.kind, core::OrderGatewayEventKind::kReady);
  EXPECT_EQ(ready.route_id, 1U);
  EXPECT_EQ(ready.ready, 1U);

  core::OrderGatewayEvent not_ready{};
  ASSERT_TRUE(shm.EventQueue(1).TryPop(&not_ready));
  EXPECT_EQ(not_ready.kind, core::OrderGatewayEventKind::kNotReady);
  EXPECT_EQ(not_ready.route_id, 1U);
  EXPECT_EQ(not_ready.ready, 0U);
}

TEST(OrderGatewayWorkerTest, GateResponseEmitsOrderResponseEvent) {
  const core::OrderGatewayShmConfig config = MakeShmConfig("order_response");
  ShmCleanup cleanup(config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(config);
  ASSERT_TRUE(shm_result.ok) << shm_result.error;
  core::OrderGatewayShmManager& shm = shm_result.value;

  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  publisher.OnOrderResponse(OrderResponse{.kind = OrderResponseKind::kRejected,
                                          .local_order_id = 1005,
                                          .exchange_order_id = 7005,
                                          .request_sequence = 55,
                                          .http_status = 503,
                                          .local_receive_ns = 800,
                                          .exchange_ns = 700,
                                          .exchange_request_ingress_ns = 701,
                                          .exchange_response_egress_ns = 702,
                                          .exchange_process_ns = 3});

  core::OrderGatewayEvent event{};
  ASSERT_TRUE(shm.EventQueue(0).TryPop(&event));
  EXPECT_EQ(event.kind, core::OrderGatewayEventKind::kOrderResponse);
  EXPECT_EQ(event.response_kind, core::OrderResponseKind::kUnknownResult);
  EXPECT_EQ(event.local_order_id, 1005U);
  EXPECT_EQ(event.exchange_order_id, 7005U);
  EXPECT_EQ(event.request_sequence, 55U);
  EXPECT_EQ(event.http_status, 503U);
  EXPECT_EQ(event.local_receive_ns, 800);
  EXPECT_EQ(event.exchange_request_ingress_ns, 701);
  EXPECT_EQ(event.exchange_response_egress_ns, 702);
  EXPECT_EQ(event.exchange_process_ns, 3);
}

}  // namespace
}  // namespace aquila::gate
