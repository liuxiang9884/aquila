#include "strategy/order_pool.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "strategy/order_types.h"

namespace aquila::strategy {
namespace {

TEST(OrderPoolTest, CreatesOrdersAndFindsByLocalId) {
  OrderPool<StrategyOrder> pool(2);

  StrategyOrder* first = pool.Create();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->local_order_id, 1);
  first->symbol_id = 7;

  StrategyOrder* second = pool.Create();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->local_order_id, 2);

  EXPECT_EQ(pool.Create(), nullptr);
  EXPECT_EQ(pool.size(), 2U);
  EXPECT_EQ(pool.capacity(), 2U);

  const StrategyOrder* found = pool.Find(1);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->symbol_id, 7);
  EXPECT_EQ(pool.Find(3), nullptr);
}

TEST(OrderPoolTest, BindsExchangeOrderIdToLocalOrder) {
  OrderPool<StrategyOrder> pool(4);
  StrategyOrder* order = pool.Create();
  ASSERT_NE(order, nullptr);

  EXPECT_TRUE(pool.BindExchangeOrderId(order->local_order_id, 360288U));
  EXPECT_EQ(order->exchange_order_id, 360288U);

  StrategyOrder* by_exchange = pool.FindByExchangeOrderId(360288U);
  ASSERT_NE(by_exchange, nullptr);
  EXPECT_EQ(by_exchange->local_order_id, order->local_order_id);
  EXPECT_FALSE(pool.BindExchangeOrderId(99, 123U));
  EXPECT_EQ(pool.FindByExchangeOrderId(123U), nullptr);
}

}  // namespace
}  // namespace aquila::strategy
