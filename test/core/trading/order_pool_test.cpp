#include "core/trading/order_pool.h"

#include <cstdint>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

#include "core/trading/order_id.h"

namespace aquila {
namespace {

struct TestOrder {
  std::uint64_t local_order_id{0};
  std::int64_t payload{0};
};

TEST(OrderPoolTest, CreatesMonotonicLocalIdsAndReportsCapacity) {
  OrderPool<TestOrder> pool(2);

  EXPECT_EQ(pool.capacity(), 2U);
  EXPECT_EQ(pool.slot_capacity(), 4U);
  EXPECT_EQ(pool.index_reserve_size(), 32U);
  EXPECT_EQ(pool.size(), 0U);

  TestOrder* first = pool.Create();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->local_order_id, 1);

  TestOrder* second = pool.Create();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->local_order_id, 2);

  EXPECT_EQ(pool.Create(), nullptr);
  EXPECT_EQ(pool.size(), 2U);
}

TEST(OrderPoolTest, EncodesStrategyIdInLocalOrderIdHighBits) {
  OrderPool<TestOrder> pool(2, 7);

  TestOrder* first = pool.Create();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(LocalOrderIdCodec::StrategyId(first->local_order_id), 7);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(first->local_order_id), 1U);

  TestOrder* second = pool.Create();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(LocalOrderIdCodec::StrategyId(second->local_order_id), 7);
  EXPECT_EQ(LocalOrderIdCodec::StrategyOrderId(second->local_order_id), 2U);
}

TEST(OrderPoolTest, IndexReserveSizeUsesLargerMultiplierForSmallPools) {
  OrderPool<TestOrder> small(1023);
  OrderPool<TestOrder> threshold(1024);
  OrderPool<TestOrder> large(2048);

  EXPECT_EQ(small.index_reserve_size(), 1023U * 16U);
  EXPECT_EQ(threshold.index_reserve_size(), 1024U * 8U);
  EXPECT_EQ(large.index_reserve_size(), 2048U * 8U);
}

TEST(OrderPoolTest, FindsOnlyLiveOrdersByLocalId) {
  OrderPool<TestOrder> pool(2);
  TestOrder* order = pool.Create();
  ASSERT_NE(order, nullptr);
  order->payload = 7;
  const std::uint64_t local_order_id = order->local_order_id;

  EXPECT_EQ(pool.Find(local_order_id), order);
  EXPECT_EQ(pool.Find(local_order_id)->payload, 7);
  EXPECT_EQ(pool.Find(0), nullptr);
  EXPECT_EQ(pool.Find(99), nullptr);

  EXPECT_TRUE(pool.Erase(local_order_id));
  EXPECT_EQ(pool.Find(local_order_id), nullptr);
  EXPECT_FALSE(pool.Erase(local_order_id));
}

TEST(OrderPoolTest, EraseReleasesSlotAndCreateReusesAddressWithNewId) {
  OrderPool<TestOrder> pool(1);
  TestOrder* first = pool.Create();
  ASSERT_NE(first, nullptr);
  first->payload = 91;
  const std::uint64_t first_id = first->local_order_id;

  EXPECT_TRUE(pool.Erase(first_id));

  TestOrder* second = pool.Create();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second, first);
  EXPECT_EQ(second->local_order_id, 2);
  EXPECT_EQ(second->payload, 0);
}

TEST(OrderPoolTest, LiveOrderPointerStaysStableAcrossOtherRecycles) {
  OrderPool<TestOrder> pool(4);
  TestOrder* stable = pool.Create();
  ASSERT_NE(stable, nullptr);
  stable->payload = 123;
  const std::uint64_t stable_id = stable->local_order_id;

  for (int i = 0; i < 16; ++i) {
    TestOrder* transient = pool.Create();
    ASSERT_NE(transient, nullptr);
    const std::uint64_t transient_id = transient->local_order_id;
    transient->payload = i;
    EXPECT_TRUE(pool.Erase(transient_id));
    EXPECT_EQ(pool.Find(stable_id), stable);
    EXPECT_EQ(stable->payload, 123);
  }
}

TEST(OrderPoolTest, ZeroCapacityNeverAllocates) {
  OrderPool<TestOrder> pool(0);

  EXPECT_EQ(pool.capacity(), 0U);
  EXPECT_EQ(pool.slot_capacity(), 0U);
  EXPECT_EQ(pool.size(), 0U);
  EXPECT_EQ(pool.Create(), nullptr);
  EXPECT_EQ(pool.Find(1), nullptr);
  EXPECT_FALSE(pool.Erase(1));
}

TEST(OrderPoolTest, RejectsCapacityTooLargeForUint32SlotIndex) {
  constexpr std::size_t kTooLargeCapacity =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) / 2 +
      1;

  EXPECT_THROW(OrderPool<TestOrder> pool(kTooLargeCapacity),
               std::invalid_argument);
}

}  // namespace
}  // namespace aquila
