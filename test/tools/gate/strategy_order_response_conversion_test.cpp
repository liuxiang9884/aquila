#include "tools/gate/strategy_order_response_conversion.h"

#include <gtest/gtest.h>

namespace aquila::tools::gate_strategy_order {
namespace {

TEST(GateStrategyOrderResponseConversionTest, CopiesLatencyTimestamps) {
  const gate::OrderResponse response{
      .kind = gate::OrderResponseKind::kAccepted,
      .local_order_id = 123,
      .group_id = 22,
      .exchange_order_id = 456,
      .route_id = 3,
      .local_receive_ns = 1770000000000000123LL,
      .exchange_ns = 1770000000000000000LL,
  };

  const core::OrderResponseEvent event = ToCoreEvent(response);

  EXPECT_EQ(event.kind, core::OrderResponseKind::kAccepted);
  EXPECT_EQ(event.local_order_id, 123U);
  EXPECT_EQ(event.group_id, 22U);
  EXPECT_EQ(event.exchange_order_id, 456U);
  EXPECT_EQ(event.route_id, 3U);
  EXPECT_EQ(event.local_receive_ns, 1770000000000000123LL);
  EXPECT_EQ(event.exchange_ns, 1770000000000000000LL);
}

TEST(GateStrategyOrderResponseConversionTest,
     ConvertsServerErrorToUnknownResult) {
  const gate::OrderResponse response{
      .kind = gate::OrderResponseKind::kRejected,
      .local_order_id = 789,
      .http_status = 500,
      .local_receive_ns = 1770000000000000456LL,
      .exchange_ns = 1770000000000000400LL,
  };

  const core::OrderResponseEvent event = ToCoreEvent(response);

  EXPECT_EQ(event.kind, core::OrderResponseKind::kUnknownResult);
  EXPECT_EQ(event.local_order_id, 789U);
  EXPECT_EQ(event.local_receive_ns, 1770000000000000456LL);
  EXPECT_EQ(event.exchange_ns, 1770000000000000400LL);
}

}  // namespace
}  // namespace aquila::tools::gate_strategy_order
