#include "core/trading/order_latency.h"

#include <gtest/gtest.h>

#include "core/trading/order_types.h"

namespace aquila::core {
namespace {

TEST(OrderLatencyTest, DeltaReturnsZeroWhenEitherSideMissing) {
  EXPECT_EQ(LatencyDeltaNs(0, 100), 0);
  EXPECT_EQ(LatencyDeltaNs(200, 0), 0);
  EXPECT_EQ(LatencyDeltaNs(0, 0), 0);
}

TEST(OrderLatencyTest, DeltaPreservesNegativeClockSkew) {
  EXPECT_EQ(LatencyDeltaNs(900, 1000), -100);
}

TEST(OrderLatencyTest, BuildsStrategyOrderTimingSnapshot) {
  StrategyOrder order{};
  order.request_send_local_ns = 1'000;
  order.ack_local_receive_ns = 1'250;
  order.response_local_receive_ns = 1'900;
  order.ack_exchange_ns = 1'100;
  order.response_exchange_ns = 1'700;
  order.accepted_exchange_ns = 1'800;
  order.finish_exchange_ns = 2'300;

  const StrategyOrderTimingSnapshot snapshot =
      MakeStrategyOrderTimingSnapshot(order);

  EXPECT_EQ(snapshot.request_send_local_ns, 1'000);
  EXPECT_EQ(snapshot.ack_rtt_ns, 250);
  EXPECT_EQ(snapshot.response_rtt_ns, 900);
  EXPECT_EQ(snapshot.ack_exchange_to_local_ns, 150);
  EXPECT_EQ(snapshot.response_exchange_to_local_ns, 200);
  EXPECT_EQ(snapshot.exchange_lifecycle_ns, 500);
}

}  // namespace
}  // namespace aquila::core
