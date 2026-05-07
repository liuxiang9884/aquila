#include "strategy/order_store.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "strategy/order_types.h"

namespace aquila::strategy {
namespace {

TEST(OrderStoreTest, CreatesOrdersAndFindsByLocalId) {
  OrderStore<StrategyOrder> store(2);

  StrategyOrder* first = store.Create();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->local_order_id, 1);
  first->symbol_id = 7;

  StrategyOrder* second = store.Create();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->local_order_id, 2);

  EXPECT_EQ(store.Create(), nullptr);
  EXPECT_EQ(store.size(), 2U);
  EXPECT_EQ(store.capacity(), 2U);

  const StrategyOrder* found = store.Find(1);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->symbol_id, 7);
  EXPECT_EQ(store.Find(3), nullptr);
}

TEST(OrderStoreTest, BindsExchangeOrderIdToLocalOrder) {
  OrderStore<StrategyOrder> store(4);
  StrategyOrder* order = store.Create();
  ASSERT_NE(order, nullptr);

  EXPECT_TRUE(store.BindExchangeOrderId(order->local_order_id, 360288U));
  EXPECT_EQ(order->exchange_order_id, 360288U);

  StrategyOrder* by_exchange = store.FindByExchangeOrderId(360288U);
  ASSERT_NE(by_exchange, nullptr);
  EXPECT_EQ(by_exchange->local_order_id, order->local_order_id);
  EXPECT_FALSE(store.BindExchangeOrderId(99, 123U));
  EXPECT_EQ(store.FindByExchangeOrderId(123U), nullptr);
}

}  // namespace
}  // namespace aquila::strategy
