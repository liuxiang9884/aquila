#include "monitor/model/monitor_spsc_queue.h"

#include <gtest/gtest.h>

namespace aquila::monitor {
namespace {

TEST(MonitorSpscQueueTest, StartsEmptyAndPreservesFifoOrder) {
  MonitorSpscQueue<int, 4> queue;
  int value = 0;

  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.TryPop(&value));

  EXPECT_TRUE(queue.TryPush(10));
  EXPECT_TRUE(queue.TryPush(20));
  EXPECT_FALSE(queue.empty());

  ASSERT_TRUE(queue.TryPop(&value));
  EXPECT_EQ(value, 10);
  ASSERT_TRUE(queue.TryPop(&value));
  EXPECT_EQ(value, 20);
  EXPECT_TRUE(queue.empty());
}

TEST(MonitorSpscQueueTest, FullQueueDropsPushesAndCountsThem) {
  MonitorSpscQueue<int, 2> queue;

  EXPECT_TRUE(queue.TryPush(1));
  EXPECT_TRUE(queue.TryPush(2));
  EXPECT_FALSE(queue.TryPush(3));
  EXPECT_FALSE(queue.TryPush(4));

  EXPECT_EQ(queue.dropped_push_count(), 2);
}

TEST(MonitorSpscQueueTest, PopAfterFullAllowsMorePushes) {
  MonitorSpscQueue<int, 2> queue;
  int value = 0;

  ASSERT_TRUE(queue.TryPush(1));
  ASSERT_TRUE(queue.TryPush(2));
  ASSERT_FALSE(queue.TryPush(3));

  ASSERT_TRUE(queue.TryPop(&value));
  EXPECT_EQ(value, 1);
  EXPECT_TRUE(queue.TryPush(3));

  ASSERT_TRUE(queue.TryPop(&value));
  EXPECT_EQ(value, 2);
  ASSERT_TRUE(queue.TryPop(&value));
  EXPECT_EQ(value, 3);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.dropped_push_count(), 1);
}

}  // namespace
}  // namespace aquila::monitor
