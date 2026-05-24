#include "tools/gate/strategy_order_response_conversion.h"

#include <gtest/gtest.h>

namespace aquila::tools::gate_strategy_order {
namespace {

TEST(GateStrategyOrderResponseConversionTest, CopiesLatencyTimestamps) {
  const gate::OrderResponse response{
      .kind = gate::OrderResponseKind::kAccepted,
      .local_order_id = 123,
      .exchange_order_id = 456,
      .local_receive_ns = 1770000000000000123LL,
      .exchange_ns = 1770000000000000000LL,
  };

  const core::OrderResponseEvent event = ToCoreEvent(response);

  EXPECT_EQ(event.kind, core::OrderResponseKind::kAccepted);
  EXPECT_EQ(event.local_order_id, 123U);
  EXPECT_EQ(event.exchange_order_id, 456U);
  EXPECT_EQ(event.local_receive_ns, 1770000000000000123LL);
  EXPECT_EQ(event.exchange_ns, 1770000000000000000LL);
}

}  // namespace
}  // namespace aquila::tools::gate_strategy_order
