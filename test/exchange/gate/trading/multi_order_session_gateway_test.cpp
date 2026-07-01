#include "exchange/gate/trading/multi_order_session_gateway.h"

#include <cstdint>
#include <vector>

#include "core/trading/order_types.h"
#include "exchange/gate/trading/order_types.h"
#include "gtest/gtest.h"

namespace aquila::gate {
namespace {

class FakeSession {
 public:
  explicit FakeSession(bool ready = true) : ready_(ready) {}

  [[nodiscard]] bool Ready() const noexcept { return ready_; }
  void set_ready(bool ready) noexcept { ready_ = ready; }

  OrderSendResult PlaceOrder(const core::StrategyOrder& order) noexcept {
    placed.push_back(order.local_order_id);
    return {.status = OrderSendStatus::kOk,
            .request_sequence = next_sequence++,
            .encoded_request_id = order.local_order_id,
            .send_local_ns = static_cast<std::int64_t>(1000 + placed.size())};
  }

  OrderSendResult CancelOrder(const core::StrategyOrder& order) noexcept {
    cancelled.push_back(order.local_order_id);
    return {.status = OrderSendStatus::kOk,
            .request_sequence = next_sequence++,
            .encoded_request_id = order.local_order_id,
            .send_local_ns = 0};
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    cached_local_ids.push_back(local_order_id);
    cached_exchange_ids.push_back(exchange_order_id);
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    forgotten_local_ids.push_back(local_order_id);
  }

  bool ready_{true};
  std::uint64_t next_sequence{1};
  std::vector<std::uint64_t> placed;
  std::vector<std::uint64_t> cancelled;
  std::vector<std::uint64_t> cached_local_ids;
  std::vector<std::uint64_t> cached_exchange_ids;
  std::vector<std::uint64_t> forgotten_local_ids;
};

core::StrategyOrder MakeOrder(std::uint64_t local_order_id,
                              std::uint16_t route_id) noexcept {
  core::StrategyOrder order;
  order.local_order_id = local_order_id;
  order.symbol_id = 1;
  order.symbol = "BTC_USDT";
  order.quantity = 1.0;
  order.quantity_text = "1";
  order.price_text = "50000";
  order.gateway_route_id = route_id;
  return order;
}

using Gateway = MultiOrderSessionGateway<FakeSession>;

std::vector<FakeSession*> MakeSessionPointers(
    std::vector<FakeSession>& sessions) {
  std::vector<FakeSession*> result;
  result.reserve(sessions.size());
  for (FakeSession& session : sessions) {
    result.push_back(&session);
  }
  return result;
}

Gateway MakeGateway(std::vector<FakeSession>& sessions) {
  return Gateway(MakeSessionPointers(sessions));
}

TEST(MultiOrderSessionGatewayTest, ExplicitRouteSendsToSelectedSession) {
  std::vector<FakeSession> sessions(4);
  Gateway gateway = MakeGateway(sessions);

  const OrderSendResult sent = gateway.PlaceOrder(MakeOrder(101, 2));

  EXPECT_EQ(sent.status, OrderSendStatus::kOk);
  EXPECT_TRUE(sessions[0].placed.empty());
  EXPECT_TRUE(sessions[1].placed.empty());
  ASSERT_EQ(sessions[2].placed.size(), 1U);
  EXPECT_EQ(sessions[2].placed[0], 101U);
  EXPECT_TRUE(sessions[3].placed.empty());
}

TEST(MultiOrderSessionGatewayTest, AutoRouteRoundRobinsAcrossReadySessions) {
  std::vector<FakeSession> sessions(4);
  Gateway gateway = MakeGateway(sessions);

  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(101, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(102, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(103, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(104, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);

  EXPECT_EQ(sessions[0].placed, std::vector<std::uint64_t>({101}));
  EXPECT_EQ(sessions[1].placed, std::vector<std::uint64_t>({102}));
  EXPECT_EQ(sessions[2].placed, std::vector<std::uint64_t>({103}));
  EXPECT_EQ(sessions[3].placed, std::vector<std::uint64_t>({104}));
}

TEST(MultiOrderSessionGatewayTest, CancelReturnsToOriginalRoute) {
  std::vector<FakeSession> sessions(4);
  Gateway gateway = MakeGateway(sessions);

  ASSERT_EQ(gateway.PlaceOrder(MakeOrder(201, 3)).status,
            OrderSendStatus::kOk);
  const OrderSendResult cancelled = gateway.CancelOrder(MakeOrder(201, 0));

  EXPECT_EQ(cancelled.status, OrderSendStatus::kOk);
  EXPECT_TRUE(sessions[0].cancelled.empty());
  EXPECT_TRUE(sessions[1].cancelled.empty());
  EXPECT_TRUE(sessions[2].cancelled.empty());
  EXPECT_EQ(sessions[3].cancelled, std::vector<std::uint64_t>({201}));
}

TEST(MultiOrderSessionGatewayTest, CacheAndForgetUseOriginalRoute) {
  std::vector<FakeSession> sessions(4);
  Gateway gateway = MakeGateway(sessions);

  ASSERT_EQ(gateway.PlaceOrder(MakeOrder(301, 1)).status,
            OrderSendStatus::kOk);
  gateway.CacheExchangeOrderId(301, 9001);
  gateway.ForgetExchangeOrderId(301);

  EXPECT_EQ(sessions[1].cached_local_ids, std::vector<std::uint64_t>({301}));
  EXPECT_EQ(sessions[1].cached_exchange_ids,
            std::vector<std::uint64_t>({9001}));
  EXPECT_EQ(sessions[1].forgotten_local_ids,
            std::vector<std::uint64_t>({301}));
  EXPECT_EQ(gateway.CancelOrder(MakeOrder(301, 1)).status,
            OrderSendStatus::kInvalidRoute);
}

TEST(MultiOrderSessionGatewayTest, InvalidRouteRejectsWithoutSending) {
  std::vector<FakeSession> sessions(4);
  Gateway gateway = MakeGateway(sessions);

  const OrderSendResult sent = gateway.PlaceOrder(MakeOrder(401, 4));

  EXPECT_EQ(sent.status, OrderSendStatus::kInvalidRoute);
  for (const FakeSession& session : sessions) {
    EXPECT_TRUE(session.placed.empty());
  }
}

TEST(MultiOrderSessionGatewayTest, RouteValidationUsesRuntimeSessionCount) {
  std::vector<FakeSession> sessions(2);
  Gateway gateway = MakeGateway(sessions);

  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(501, 1)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(502, 2)).status,
            OrderSendStatus::kInvalidRoute);
  EXPECT_EQ(sessions[1].placed, std::vector<std::uint64_t>({501}));
}

TEST(MultiOrderSessionGatewayTest, RouteTableCapacityRejectsBeforeSending) {
  std::vector<FakeSession> sessions(2);
  Gateway::Config config;
  config.route_table_capacity = 1;
  Gateway gateway(MakeSessionPointers(sessions), config);

  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(701, 0)).status,
            OrderSendStatus::kOk);
  const OrderSendResult rejected = gateway.PlaceOrder(MakeOrder(702, 1));

  EXPECT_EQ(rejected.status, OrderSendStatus::kInflightFull);
  EXPECT_EQ(sessions[0].placed, std::vector<std::uint64_t>({701}));
  EXPECT_TRUE(sessions[1].placed.empty());
}

TEST(MultiOrderSessionGatewayTest, ReadyRequiresConfiguredMinimum) {
  std::vector<FakeSession> sessions{FakeSession{}, FakeSession{},
                                    FakeSession{false}, FakeSession{false}};
  Gateway::Config config;
  config.min_ready_sessions = 3;
  Gateway gateway(MakeSessionPointers(sessions), config);

  EXPECT_FALSE(gateway.Ready());
  sessions[2].set_ready(true);
  EXPECT_TRUE(gateway.Ready());
}

TEST(MultiOrderSessionGatewayTest, ExposesFanoutLimitAndRouteReadiness) {
  std::vector<FakeSession> sessions{FakeSession{}, FakeSession{false},
                                    FakeSession{}, FakeSession{false}};
  Gateway gateway = MakeGateway(sessions);

  EXPECT_EQ(gateway.MaxOrderSessionFanout(), 4U);
  EXPECT_TRUE(gateway.RouteReady(0));
  EXPECT_FALSE(gateway.RouteReady(1));
  EXPECT_TRUE(gateway.RouteReady(2));
  EXPECT_FALSE(gateway.RouteReady(3));
  EXPECT_FALSE(gateway.RouteReady(4));

  sessions[1].set_ready(true);
  EXPECT_TRUE(gateway.RouteReady(1));
}

TEST(MultiOrderSessionGatewayTest, AutoRouteSkipsNotReadySessions) {
  std::vector<FakeSession> sessions{FakeSession{}, FakeSession{false},
                                    FakeSession{}, FakeSession{false}};
  Gateway gateway = MakeGateway(sessions);

  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(601, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(602, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);

  EXPECT_EQ(sessions[0].placed, std::vector<std::uint64_t>({601}));
  EXPECT_TRUE(sessions[1].placed.empty());
  EXPECT_EQ(sessions[2].placed, std::vector<std::uint64_t>({602}));
  EXPECT_TRUE(sessions[3].placed.empty());
}

}  // namespace
}  // namespace aquila::gate
