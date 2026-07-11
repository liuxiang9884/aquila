#include "exchange/bitget/trading/multi_order_session_gateway.h"

#include <cstdint>
#include <vector>

#include "core/trading/order_types.h"
#include "exchange/bitget/trading/order_types.h"
#include "gtest/gtest.h"

namespace aquila::bitget {
namespace {

class FakeSession {
 public:
  explicit FakeSession(bool ready = true) : ready_(ready) {}

  [[nodiscard]] bool Ready() const noexcept {
    return ready_;
  }
  void set_ready(bool ready) noexcept {
    ready_ = ready;
  }

  OrderSendResult PlaceOrder(const core::StrategyOrder& order) noexcept {
    placed.push_back(order.local_order_id);
    return {.status = OrderSendStatus::kOk,
            .request_sequence = next_sequence++,
            .encoded_request_id = order.local_order_id,
            .send_local_ns = static_cast<std::int64_t>(1000 + placed.size())};
  }

  OrderSendResult CancelOrder(const core::StrategyOrder& order) noexcept {
    cancelled.push_back(order.local_order_id);
    return {
        .status = OrderSendStatus::kOk,
        .request_sequence = next_sequence++,
        .encoded_request_id = order.local_order_id,
        .send_local_ns = static_cast<std::int64_t>(2000 + cancelled.size())};
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
  order.exchange = Exchange::kBitget;
  order.symbol_id = 1;
  order.symbol = "BTCUSDT";
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
  EXPECT_EQ(sessions[2].placed, std::vector<std::uint64_t>({101}));
  EXPECT_TRUE(sessions[3].placed.empty());
}

TEST(MultiOrderSessionGatewayTest, AutoRouteRoundRobinsReadySessions) {
  std::vector<FakeSession> sessions{FakeSession{}, FakeSession{false},
                                    FakeSession{}, FakeSession{false}};
  Gateway gateway = MakeGateway(sessions);

  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(201, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(202, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(sessions[0].placed, std::vector<std::uint64_t>({201}));
  EXPECT_EQ(sessions[2].placed, std::vector<std::uint64_t>({202}));
}

TEST(MultiOrderSessionGatewayTest, CancelReturnsToOriginalRoute) {
  std::vector<FakeSession> sessions(4);
  Gateway gateway = MakeGateway(sessions);

  ASSERT_EQ(gateway.PlaceOrder(MakeOrder(301, 3)).status, OrderSendStatus::kOk);
  EXPECT_EQ(gateway.CancelOrder(MakeOrder(301, 0)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(sessions[3].cancelled, std::vector<std::uint64_t>({301}));
}

TEST(MultiOrderSessionGatewayTest, CacheAndForgetUseOriginalRoute) {
  std::vector<FakeSession> sessions(2);
  Gateway gateway = MakeGateway(sessions);

  ASSERT_EQ(gateway.PlaceOrder(MakeOrder(401, 1)).status, OrderSendStatus::kOk);
  gateway.CacheExchangeOrderId(401, 9001);
  gateway.ForgetExchangeOrderId(401);

  EXPECT_EQ(sessions[1].cached_local_ids, std::vector<std::uint64_t>({401}));
  EXPECT_EQ(sessions[1].cached_exchange_ids,
            std::vector<std::uint64_t>({9001}));
  EXPECT_EQ(sessions[1].forgotten_local_ids, std::vector<std::uint64_t>({401}));
  EXPECT_EQ(gateway.CancelOrder(MakeOrder(401, 1)).status,
            OrderSendStatus::kInvalidRoute);
}

TEST(MultiOrderSessionGatewayTest, InvalidRouteRejectsWithoutSending) {
  std::vector<FakeSession> sessions(2);
  Gateway gateway = MakeGateway(sessions);

  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(501, 2)).status,
            OrderSendStatus::kInvalidRoute);
  EXPECT_TRUE(sessions[0].placed.empty());
  EXPECT_TRUE(sessions[1].placed.empty());
}

TEST(MultiOrderSessionGatewayTest, RouteTableCapacityRejectsBeforeSending) {
  std::vector<FakeSession> sessions(2);
  Gateway::Config config;
  config.route_table_capacity = 1;
  Gateway gateway(MakeSessionPointers(sessions), config);

  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(601, 0)).status, OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(602, 1)).status,
            OrderSendStatus::kInflightFull);
  EXPECT_TRUE(sessions[1].placed.empty());
}

TEST(MultiOrderSessionGatewayTest, FailedPlaceRollsBackRecordedRoute) {
  class RejectingSession : public FakeSession {
   public:
    using FakeSession::FakeSession;
    OrderSendResult PlaceOrder(const core::StrategyOrder&) noexcept {
      return {.status = OrderSendStatus::kWriteUnavailable};
    }
  } rejecting;
  std::vector<RejectingSession*> sessions{&rejecting};
  MultiOrderSessionGateway<RejectingSession> gateway(sessions);

  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(701, 0)).status,
            OrderSendStatus::kWriteUnavailable);
  EXPECT_EQ(gateway.CancelOrder(MakeOrder(701, 0)).status,
            OrderSendStatus::kInvalidRoute);
}

TEST(MultiOrderSessionGatewayTest, ReadyRequiresConfiguredMinimum) {
  std::vector<FakeSession> sessions{FakeSession{}, FakeSession{},
                                    FakeSession{false}};
  Gateway::Config config;
  config.min_ready_sessions = 3;
  Gateway gateway(MakeSessionPointers(sessions), config);

  EXPECT_FALSE(gateway.Ready());
  sessions[2].set_ready(true);
  EXPECT_TRUE(gateway.Ready());
}

TEST(MultiOrderSessionGatewayTest, ExposesFanoutAndRouteReadiness) {
  std::vector<FakeSession> sessions{FakeSession{}, FakeSession{false}};
  Gateway gateway = MakeGateway(sessions);

  EXPECT_EQ(gateway.MaxOrderSessionFanout(), 2U);
  EXPECT_TRUE(gateway.RouteReady(0));
  EXPECT_FALSE(gateway.RouteReady(1));
  EXPECT_FALSE(gateway.RouteReady(2));
}

}  // namespace
}  // namespace aquila::bitget
