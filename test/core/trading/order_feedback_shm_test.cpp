#include "core/trading/order_feedback_shm.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/trading/order_id.h"

namespace aquila {
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
  return fmt::format("/aquila_order_feedback_shm_test_{}_{}", ::getpid(),
                     suffix);
}

OrderFeedbackShmConfig MakeCreateConfig(std::string_view suffix) {
  return OrderFeedbackShmConfig{
      .shm_name = UniqueShmName(suffix),
      .channel_name = "orders",
      .create = true,
      .remove_existing = true,
  };
}

OrderFeedbackShmConfig MakeOpenConfig(
    const OrderFeedbackShmConfig& create_config) {
  OrderFeedbackShmConfig config = create_config;
  config.create = false;
  config.remove_existing = false;
  return config;
}

template <typename Mutator>
void ExpectOpenFailsAfterHeaderMutation(std::string_view suffix,
                                        Mutator mutator) {
  const OrderFeedbackShmConfig config = MakeCreateConfig(suffix);
  ShmCleanup cleanup(config.shm_name);

  auto create_result = OrderFeedbackShmManager::Create(config);
  ASSERT_TRUE(create_result.ok) << create_result.error;
  mutator(create_result.value.channel().header);

  auto open_result = OrderFeedbackShmManager::Open(MakeOpenConfig(config));
  EXPECT_FALSE(open_result.ok);
}

OrderFeedbackEvent MakeAcceptedEvent(std::uint8_t strategy_id,
                                     std::uint64_t strategy_order_id) {
  OrderFeedbackEvent event{};
  event.kind = OrderFeedbackKind::kAccepted;
  event.local_order_id =
      LocalOrderIdCodec::Encode(strategy_id, strategy_order_id);
  event.exchange_order_id = 9000 + strategy_order_id;
  event.cumulative_filled_quantity = 10;
  event.left_quantity = 90;
  event.cancelled_quantity = 0;
  event.fill_price = 1234.5;
  event.role = OrderRole::kMaker;
  event.exchange_update_ns = 111;
  event.local_receive_ns = 222;
  return event;
}

bool Pop(OrderFeedbackLane& lane, OrderFeedbackEvent& event) {
  return lane.queue.TryPop(event);
}

std::size_t FillLaneUntilFull(OrderFeedbackLane& lane) {
  std::size_t pushed = 0;
  for (;;) {
    OrderFeedbackEvent event =
        MakeAcceptedEvent(lane.header.strategy_id, pushed + 1);
    if (!lane.queue.TryPush(event)) {
      return pushed;
    }
    ++pushed;
  }
}

std::unique_ptr<OrderFeedbackShmChannel> MakeChannelForTest() {
  auto channel = std::make_unique<OrderFeedbackShmChannel>();
  order_feedback_shm_detail::InitializeLaneHeaders(*channel);
  return channel;
}

TEST(OrderFeedbackShmTest, CreateInitializesHeaderAndLanes) {
  const OrderFeedbackShmConfig config = MakeCreateConfig("create_init");
  ShmCleanup cleanup(config.shm_name);

  auto manager_result = OrderFeedbackShmManager::Create(config);
  ASSERT_TRUE(manager_result.ok) << manager_result.error;
  const OrderFeedbackShmChannel& channel = manager_result.value.channel();

  EXPECT_EQ(channel.header.magic, kOrderFeedbackShmMagic);
  EXPECT_EQ(channel.header.version, kOrderFeedbackShmVersion);
  EXPECT_EQ(channel.header.abi_size, sizeof(OrderFeedbackEvent));
  EXPECT_EQ(channel.header.event_size, sizeof(OrderFeedbackEvent));
  EXPECT_EQ(channel.header.max_strategy_count, kMaxOrderFeedbackStrategies);
  EXPECT_EQ(channel.header.queue_capacity, kOrderFeedbackQueueCapacity);
  EXPECT_NE(channel.header.producer_pid, 0U);
  EXPECT_EQ(channel.header.producer_run_id, 0U);
  EXPECT_EQ(channel.header.invalid_route_count.load(std::memory_order_relaxed),
            0U);

  for (std::uint32_t i = 0; i < kMaxOrderFeedbackStrategies; ++i) {
    const OrderFeedbackLaneHeader& lane_header = channel.lanes[i].header;
    EXPECT_EQ(lane_header.strategy_id, i);
    EXPECT_EQ(lane_header.consumer_pid.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(lane_header.consumer_run_id.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(lane_header.queue_full_count.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(lane_header.dropped_count.load(std::memory_order_relaxed), 0U);
  }
}

TEST(OrderFeedbackShmTest, OpenFindsExistingChannelAndValidatesHeader) {
  const OrderFeedbackShmConfig config = MakeCreateConfig("open_existing");
  ShmCleanup cleanup(config.shm_name);

  auto create_result = OrderFeedbackShmManager::Create(config);
  ASSERT_TRUE(create_result.ok) << create_result.error;
  create_result.value.channel().header.producer_run_id = 1234;
  create_result.value.channel().lanes[3].header.consumer_run_id.store(
      5678, std::memory_order_relaxed);

  auto open_result = OrderFeedbackShmManager::Open(MakeOpenConfig(config));
  ASSERT_TRUE(open_result.ok) << open_result.error;

  EXPECT_EQ(open_result.value.channel().header.producer_run_id, 1234U);
  EXPECT_EQ(open_result.value.channel().lanes[3].header.strategy_id, 3U);
  EXPECT_EQ(open_result.value.channel().lanes[3].header.consumer_run_id.load(
                std::memory_order_relaxed),
            5678U);
}

TEST(OrderFeedbackShmTest, RejectsMissingSharedMemoryOnOpen) {
  const OrderFeedbackShmConfig config{
      .shm_name = UniqueShmName("missing_open"),
      .channel_name = "orders",
      .create = false,
      .remove_existing = false,
  };
  ShmCleanup cleanup(config.shm_name);

  auto result = OrderFeedbackShmManager::Open(config);
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.error.empty());
}

TEST(OrderFeedbackShmTest, RejectsMissingChannelOnOpen) {
  OrderFeedbackShmConfig create_config = MakeCreateConfig("missing_channel");
  create_config.channel_name = "other_orders";
  ShmCleanup cleanup(create_config.shm_name);
  auto create_result = OrderFeedbackShmManager::Create(create_config);
  ASSERT_TRUE(create_result.ok) << create_result.error;

  OrderFeedbackShmConfig open_config = MakeOpenConfig(create_config);
  open_config.channel_name = "orders";
  auto result = OrderFeedbackShmManager::Open(open_config);
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("channel_name not found"), std::string::npos);
}

TEST(OrderFeedbackShmTest, RejectsInvalidConfig) {
  auto missing_shm_result =
      OrderFeedbackShmManager::Create(OrderFeedbackShmConfig{
          .shm_name = "",
          .channel_name = "orders",
          .create = true,
          .remove_existing = false,
      });
  EXPECT_FALSE(missing_shm_result.ok);
  EXPECT_NE(missing_shm_result.error.find("shm_name is required"),
            std::string::npos);

  auto empty_channel_result =
      OrderFeedbackShmManager::Create(OrderFeedbackShmConfig{
          .shm_name = UniqueShmName("empty_channel"),
          .channel_name = "",
          .create = true,
          .remove_existing = false,
      });
  EXPECT_FALSE(empty_channel_result.ok);
  EXPECT_NE(empty_channel_result.error.find("channel_name is required"),
            std::string::npos);

  auto remove_without_create_result =
      OrderFeedbackShmManager::OpenOrCreate(OrderFeedbackShmConfig{
          .shm_name = UniqueShmName("remove_without_create"),
          .channel_name = "orders",
          .create = false,
          .remove_existing = true,
      });
  EXPECT_FALSE(remove_without_create_result.ok);
  EXPECT_NE(remove_without_create_result.error.find(
                "remove_existing requires create=true"),
            std::string::npos);
}

TEST(OrderFeedbackShmTest, RejectsHeaderMismatch) {
  ExpectOpenFailsAfterHeaderMutation(
      "magic_mismatch",
      [](OrderFeedbackShmHeader& header) { header.magic = 0; });
  ExpectOpenFailsAfterHeaderMutation(
      "version_mismatch",
      [](OrderFeedbackShmHeader& header) { header.version = 2; });
  ExpectOpenFailsAfterHeaderMutation(
      "abi_size_mismatch", [](OrderFeedbackShmHeader& header) {
        header.abi_size = sizeof(OrderFeedbackEvent) + 1;
      });
  ExpectOpenFailsAfterHeaderMutation(
      "event_size_mismatch", [](OrderFeedbackShmHeader& header) {
        header.event_size = sizeof(OrderFeedbackEvent) + 1;
      });
  ExpectOpenFailsAfterHeaderMutation(
      "strategy_count_mismatch", [](OrderFeedbackShmHeader& header) {
        header.max_strategy_count = kMaxOrderFeedbackStrategies - 1;
      });
  ExpectOpenFailsAfterHeaderMutation(
      "queue_capacity_mismatch", [](OrderFeedbackShmHeader& header) {
        header.queue_capacity = kOrderFeedbackQueueCapacity / 2;
      });
}

TEST(OrderFeedbackShmTest, QueueCapacityAndTypeConstraints) {
  static_assert(kMaxOrderFeedbackStrategies == 8);
  static_assert(kOrderFeedbackQueueCapacity == 65536);
  static_assert(
      (kOrderFeedbackQueueCapacity & (kOrderFeedbackQueueCapacity - 1)) == 0);
  static_assert(std::is_standard_layout_v<OrderFeedbackShmHeader>);
  static_assert(std::is_standard_layout_v<OrderFeedbackLaneHeader>);
  static_assert(std::is_standard_layout_v<OrderFeedbackLane>);
  static_assert(std::is_standard_layout_v<OrderFeedbackShmChannel>);

  EXPECT_EQ(kMaxOrderFeedbackStrategies, 8U);
  EXPECT_EQ(kOrderFeedbackQueueCapacity, 65536U);
  EXPECT_EQ(OrderFeedbackShmManager::StorageSize(),
            sizeof(OrderFeedbackShmChannel));
}

TEST(OrderFeedbackShmTest, PublisherRoutesOrderEventToStrategyLane) {
  auto channel = MakeChannelForTest();
  OrderFeedbackShmPublisher publisher(*channel);

  const OrderFeedbackEvent event = MakeAcceptedEvent(2, 42);

  EXPECT_TRUE(publisher.Publish(event));
  EXPECT_EQ(publisher.published_count(), 1U);

  OrderFeedbackEvent popped{};
  EXPECT_TRUE(Pop(channel->lanes[2], popped));
  EXPECT_EQ(popped.kind, event.kind);
  EXPECT_EQ(popped.local_order_id, event.local_order_id);
  EXPECT_EQ(popped.exchange_order_id, event.exchange_order_id);
  EXPECT_EQ(popped.cumulative_filled_quantity,
            event.cumulative_filled_quantity);
  EXPECT_EQ(popped.left_quantity, event.left_quantity);
  EXPECT_EQ(popped.cancelled_quantity, event.cancelled_quantity);
  EXPECT_DOUBLE_EQ(popped.fill_price, event.fill_price);
  EXPECT_EQ(popped.role, event.role);
  EXPECT_EQ(popped.exchange_update_ns, event.exchange_update_ns);
  EXPECT_EQ(popped.local_receive_ns, event.local_receive_ns);

  for (std::uint32_t i = 0; i < kMaxOrderFeedbackStrategies; ++i) {
    if (i == 2) {
      continue;
    }
    EXPECT_FALSE(Pop(channel->lanes[i], popped)) << "lane=" << i;
  }
}

TEST(OrderFeedbackShmTest, PublisherRejectsInvalidRoute) {
  auto channel = MakeChannelForTest();
  OrderFeedbackShmPublisher publisher(*channel);

  OrderFeedbackEvent gap_event{};
  gap_event.kind = OrderFeedbackKind::kGap;
  gap_event.gap_scope = OrderFeedbackGapScope::kLane;
  gap_event.gap_reason = OrderFeedbackGapReason::kLaneQueueFull;

  OrderFeedbackEvent zero_order_event = MakeAcceptedEvent(1, 1);
  zero_order_event.local_order_id = 0;

  OrderFeedbackEvent invalid_strategy_event = MakeAcceptedEvent(9, 1);

  EXPECT_FALSE(publisher.Publish(gap_event));
  EXPECT_FALSE(publisher.Publish(zero_order_event));
  EXPECT_FALSE(publisher.Publish(invalid_strategy_event));

  EXPECT_EQ(publisher.invalid_route_count(), 3U);
  EXPECT_EQ(channel->header.invalid_route_count.load(std::memory_order_relaxed),
            3U);
  EXPECT_EQ(publisher.published_count(), 0U);
}

TEST(OrderFeedbackShmTest,
     PublisherQueueFullSetsPendingLaneGapAndDoesNotAffectOtherLane) {
  auto channel = MakeChannelForTest();
  OrderFeedbackShmPublisher publisher(*channel);

  const std::size_t filled = FillLaneUntilFull(channel->lanes[3]);
  EXPECT_GT(filled, 0U);

  OrderFeedbackEvent lane3_event = MakeAcceptedEvent(3, 77);
  lane3_event.local_receive_ns = 333;
  EXPECT_FALSE(publisher.Publish(lane3_event));
  EXPECT_EQ(
      channel->lanes[3].header.queue_full_count.load(std::memory_order_relaxed),
      1U);
  EXPECT_EQ(
      channel->lanes[3].header.dropped_count.load(std::memory_order_relaxed),
      1U);

  const OrderFeedbackEvent lane4_event = MakeAcceptedEvent(4, 88);
  EXPECT_TRUE(publisher.Publish(lane4_event));

  OrderFeedbackEvent popped{};
  EXPECT_TRUE(Pop(channel->lanes[4], popped));
  EXPECT_EQ(popped.local_order_id, lane4_event.local_order_id);
}

TEST(OrderFeedbackShmTest, PendingLaneGapFlushesBeforeNextOrderEvent) {
  auto channel = MakeChannelForTest();
  OrderFeedbackShmPublisher publisher(*channel);

  EXPECT_GT(FillLaneUntilFull(channel->lanes[3]), 0U);

  OrderFeedbackEvent first_event = MakeAcceptedEvent(3, 101);
  first_event.local_receive_ns = 123;
  EXPECT_FALSE(publisher.Publish(first_event));

  OrderFeedbackEvent popped{};
  EXPECT_TRUE(Pop(channel->lanes[3], popped));
  EXPECT_TRUE(Pop(channel->lanes[3], popped));

  const OrderFeedbackEvent next_event = MakeAcceptedEvent(3, 102);
  EXPECT_TRUE(publisher.Publish(next_event));

  bool saw_gap = false;
  bool saw_next_event = false;
  while (Pop(channel->lanes[3], popped)) {
    if (!saw_gap && popped.kind == OrderFeedbackKind::kGap) {
      EXPECT_EQ(popped.gap_scope, OrderFeedbackGapScope::kLane);
      EXPECT_EQ(popped.gap_reason, OrderFeedbackGapReason::kLaneQueueFull);
      EXPECT_EQ(popped.local_receive_ns, first_event.local_receive_ns);
      saw_gap = true;
      continue;
    }

    if (saw_gap && popped.local_order_id == next_event.local_order_id) {
      EXPECT_EQ(popped.kind, OrderFeedbackKind::kAccepted);
      saw_next_event = true;
      break;
    }

    EXPECT_FALSE(saw_gap) << "ordinary event crossed pending gap";
  }

  EXPECT_TRUE(saw_gap);
  EXPECT_TRUE(saw_next_event);
}

TEST(OrderFeedbackShmTest,
     PendingLaneGapCountsNewDropWhenFlushStillFullAndPreservesOriginalGap) {
  auto channel = MakeChannelForTest();
  OrderFeedbackShmPublisher publisher(*channel);

  EXPECT_GT(FillLaneUntilFull(channel->lanes[3]), 0U);

  OrderFeedbackEvent event_a = MakeAcceptedEvent(3, 100001);
  event_a.local_receive_ns = 1111;
  EXPECT_FALSE(publisher.Publish(event_a));
  EXPECT_EQ(
      channel->lanes[3].header.queue_full_count.load(std::memory_order_relaxed),
      1U);
  EXPECT_EQ(
      channel->lanes[3].header.dropped_count.load(std::memory_order_relaxed),
      1U);

  OrderFeedbackEvent event_b = MakeAcceptedEvent(3, 100002);
  event_b.local_receive_ns = 2222;
  EXPECT_FALSE(publisher.Publish(event_b));
  EXPECT_EQ(
      channel->lanes[3].header.queue_full_count.load(std::memory_order_relaxed),
      2U);
  EXPECT_EQ(
      channel->lanes[3].header.dropped_count.load(std::memory_order_relaxed),
      2U);

  OrderFeedbackEvent popped{};
  EXPECT_TRUE(Pop(channel->lanes[3], popped));
  EXPECT_EQ(publisher.FlushPendingGapEvents(), 1U);

  bool saw_gap = false;
  while (Pop(channel->lanes[3], popped)) {
    ASSERT_NE(popped.local_order_id, event_b.local_order_id)
        << "event B crossed the pending gap";
    if (popped.kind != OrderFeedbackKind::kGap) {
      continue;
    }

    EXPECT_EQ(popped.gap_scope, OrderFeedbackGapScope::kLane);
    EXPECT_EQ(popped.gap_reason, OrderFeedbackGapReason::kLaneQueueFull);
    EXPECT_EQ(popped.local_receive_ns, event_a.local_receive_ns);
    saw_gap = true;
    break;
  }
  EXPECT_TRUE(saw_gap);
}

TEST(OrderFeedbackShmTest, PublishGlobalGapWritesAllLanes) {
  auto channel = MakeChannelForTest();
  OrderFeedbackShmPublisher publisher(*channel);

  EXPECT_TRUE(publisher.PublishGlobalGap(
      OrderFeedbackGapReason::kSessionDisconnected, 123));
  EXPECT_EQ(publisher.published_count(), kMaxOrderFeedbackStrategies);

  std::uint64_t gap_sequence = 0;
  for (std::uint32_t i = 0; i < kMaxOrderFeedbackStrategies; ++i) {
    OrderFeedbackEvent popped{};
    EXPECT_TRUE(Pop(channel->lanes[i], popped)) << "lane=" << i;
    EXPECT_EQ(popped.kind, OrderFeedbackKind::kGap);
    EXPECT_EQ(popped.gap_scope, OrderFeedbackGapScope::kGlobal);
    EXPECT_EQ(popped.gap_reason, OrderFeedbackGapReason::kSessionDisconnected);
    EXPECT_EQ(popped.local_order_id, 0U);
    EXPECT_EQ(popped.local_receive_ns, 123);
    if (i == 0) {
      gap_sequence = popped.gap_sequence;
    } else {
      EXPECT_EQ(popped.gap_sequence, gap_sequence);
    }
    EXPECT_FALSE(Pop(channel->lanes[i], popped)) << "lane=" << i;
  }
}

TEST(OrderFeedbackShmTest, PublishGlobalGapDefersFullLaneOnly) {
  auto channel = MakeChannelForTest();
  OrderFeedbackShmPublisher publisher(*channel);

  EXPECT_GT(FillLaneUntilFull(channel->lanes[5]), 0U);

  EXPECT_FALSE(publisher.PublishGlobalGap(
      OrderFeedbackGapReason::kSessionDisconnected, 123));

  std::uint64_t gap_sequence = 0;
  for (std::uint32_t i = 0; i < kMaxOrderFeedbackStrategies; ++i) {
    if (i == 5) {
      continue;
    }
    OrderFeedbackEvent popped{};
    EXPECT_TRUE(Pop(channel->lanes[i], popped)) << "lane=" << i;
    EXPECT_EQ(popped.kind, OrderFeedbackKind::kGap);
    EXPECT_EQ(popped.gap_scope, OrderFeedbackGapScope::kGlobal);
    EXPECT_EQ(popped.gap_reason, OrderFeedbackGapReason::kSessionDisconnected);
    if (gap_sequence == 0) {
      gap_sequence = popped.gap_sequence;
    } else {
      EXPECT_EQ(popped.gap_sequence, gap_sequence);
    }
  }

  OrderFeedbackEvent popped{};
  EXPECT_TRUE(Pop(channel->lanes[5], popped));
  EXPECT_EQ(publisher.FlushPendingGapEvents(), 1U);

  bool saw_global_gap = false;
  while (Pop(channel->lanes[5], popped)) {
    if (popped.kind != OrderFeedbackKind::kGap) {
      continue;
    }
    EXPECT_EQ(popped.gap_scope, OrderFeedbackGapScope::kGlobal);
    EXPECT_EQ(popped.gap_reason, OrderFeedbackGapReason::kSessionDisconnected);
    EXPECT_EQ(popped.local_receive_ns, 123);
    EXPECT_EQ(popped.gap_sequence, gap_sequence);
    saw_global_gap = true;
    break;
  }
  EXPECT_TRUE(saw_global_gap);
}

TEST(OrderFeedbackShmTest, GlobalPendingOverridesLanePending) {
  auto channel = MakeChannelForTest();
  OrderFeedbackShmPublisher publisher(*channel);

  EXPECT_GT(FillLaneUntilFull(channel->lanes[6]), 0U);

  OrderFeedbackEvent lane_event = MakeAcceptedEvent(6, 201);
  EXPECT_FALSE(publisher.Publish(lane_event));

  EXPECT_FALSE(publisher.PublishGlobalGap(
      OrderFeedbackGapReason::kReconnectUnknownWindow, 456));

  OrderFeedbackEvent popped{};
  EXPECT_TRUE(Pop(channel->lanes[6], popped));
  EXPECT_EQ(publisher.FlushPendingGapEvents(), 1U);

  bool saw_gap = false;
  while (Pop(channel->lanes[6], popped)) {
    if (popped.kind != OrderFeedbackKind::kGap) {
      continue;
    }
    EXPECT_EQ(popped.gap_scope, OrderFeedbackGapScope::kGlobal);
    EXPECT_EQ(popped.gap_reason,
              OrderFeedbackGapReason::kReconnectUnknownWindow);
    EXPECT_EQ(popped.local_receive_ns, 456);
    saw_gap = true;
    break;
  }
  EXPECT_TRUE(saw_gap);
}

TEST(OrderFeedbackShmTest, ReaderClaimRejectsInvalidStrategyId) {
  auto channel = MakeChannelForTest();

  auto result = OrderFeedbackShmReader::Claim(
      *channel, static_cast<std::uint8_t>(kMaxOrderFeedbackStrategies), 123);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("strategy_id"), std::string::npos);
}

TEST(OrderFeedbackShmTest, ReaderClaimRejectsZeroRunId) {
  auto channel = MakeChannelForTest();

  auto normal = OrderFeedbackShmReader::Claim(*channel, 2, 0);
  auto forced = OrderFeedbackShmReader::Claim(*channel, 2, 0, true);

  EXPECT_FALSE(normal.ok);
  EXPECT_NE(normal.error.find("consumer_run_id"), std::string::npos);
  EXPECT_FALSE(forced.ok);
  EXPECT_NE(forced.error.find("consumer_run_id"), std::string::npos);
  EXPECT_EQ(
      channel->lanes[2].header.consumer_pid.load(std::memory_order_relaxed),
      0U);
  EXPECT_EQ(
      channel->lanes[2].header.consumer_run_id.load(std::memory_order_relaxed),
      0U);
}

TEST(OrderFeedbackShmTest, ReaderClaimUsesRunIdAsOwnershipToken) {
  auto channel = MakeChannelForTest();
  OrderFeedbackLaneHeader& header = channel->lanes[5].header;
  header.consumer_pid.store(999999, std::memory_order_relaxed);
  header.consumer_run_id.store(0, std::memory_order_relaxed);

  auto result = OrderFeedbackShmReader::Claim(*channel, 5, 101);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 101U);
  EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed),
            static_cast<std::uint64_t>(::getpid()));

  result.value.Release();

  EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed), 0U);
}

TEST(OrderFeedbackShmTest, ReaderClaimStoresOwnerAndReleaseClearsOwner) {
  auto channel = MakeChannelForTest();

  auto result = OrderFeedbackShmReader::Claim(*channel, 2, 44);

  ASSERT_TRUE(result.ok) << result.error;
  OrderFeedbackLaneHeader& header = channel->lanes[2].header;
  EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed),
            static_cast<std::uint64_t>(::getpid()));
  EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 44U);

  result.value.Release();

  EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 0U);
}

TEST(OrderFeedbackShmTest, ReaderClaimRejectsDuplicateLiveOwnerWithoutForce) {
  auto channel = MakeChannelForTest();

  auto first = OrderFeedbackShmReader::Claim(*channel, 1, 11);
  ASSERT_TRUE(first.ok) << first.error;

  auto second = OrderFeedbackShmReader::Claim(*channel, 1, 22);

  EXPECT_FALSE(second.ok);
  EXPECT_NE(second.error.find("already claimed"), std::string::npos);
  EXPECT_EQ(
      channel->lanes[1].header.consumer_pid.load(std::memory_order_relaxed),
      static_cast<std::uint64_t>(::getpid()));
  EXPECT_EQ(
      channel->lanes[1].header.consumer_run_id.load(std::memory_order_relaxed),
      11U);
}

TEST(OrderFeedbackShmTest, ReaderForceClaimOverridesOwner) {
  auto channel = MakeChannelForTest();

  auto first = OrderFeedbackShmReader::Claim(*channel, 1, 11);
  ASSERT_TRUE(first.ok) << first.error;
  auto second = OrderFeedbackShmReader::Claim(*channel, 1, 22, true);
  ASSERT_TRUE(second.ok) << second.error;

  OrderFeedbackLaneHeader& header = channel->lanes[1].header;
  EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed),
            static_cast<std::uint64_t>(::getpid()));
  EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 22U);
}

TEST(OrderFeedbackShmTest, ReaderReleaseAfterForceClaimDoesNotClearNewOwner) {
  auto channel = MakeChannelForTest();

  auto first = OrderFeedbackShmReader::Claim(*channel, 1, 11);
  ASSERT_TRUE(first.ok) << first.error;
  auto second = OrderFeedbackShmReader::Claim(*channel, 1, 22, true);
  ASSERT_TRUE(second.ok) << second.error;

  OrderFeedbackLaneHeader& header = channel->lanes[1].header;
  first.value.Release();

  EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed),
            static_cast<std::uint64_t>(::getpid()));
  EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 22U);

  second.value.Release();

  EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 0U);
}

TEST(OrderFeedbackShmTest, ReaderDestructorReleasesOwnedLane) {
  auto channel = MakeChannelForTest();
  OrderFeedbackLaneHeader& header = channel->lanes[3].header;

  {
    auto result = OrderFeedbackShmReader::Claim(*channel, 3, 55);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed),
              static_cast<std::uint64_t>(::getpid()));
    EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 55U);
  }

  EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 0U);
}

TEST(OrderFeedbackShmTest, ReaderMoveTransfersOwnership) {
  auto channel = MakeChannelForTest();
  OrderFeedbackLaneHeader& header = channel->lanes[4].header;

  auto result = OrderFeedbackShmReader::Claim(*channel, 4, 66);
  ASSERT_TRUE(result.ok) << result.error;
  OrderFeedbackShmReader moved(std::move(result.value));

  result.value.Release();

  EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed),
            static_cast<std::uint64_t>(::getpid()));
  EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 66U);

  moved.Release();

  EXPECT_EQ(header.consumer_pid.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(header.consumer_run_id.load(std::memory_order_relaxed), 0U);
}

TEST(OrderFeedbackShmTest, ReaderPollDrainsFifoWithinBudget) {
  auto channel = MakeChannelForTest();
  ASSERT_TRUE(channel->lanes[2].queue.TryPush(MakeAcceptedEvent(2, 1)));
  ASSERT_TRUE(channel->lanes[2].queue.TryPush(MakeAcceptedEvent(2, 2)));
  ASSERT_TRUE(channel->lanes[2].queue.TryPush(MakeAcceptedEvent(2, 3)));

  auto result = OrderFeedbackShmReader::Claim(*channel, 2, 77);
  ASSERT_TRUE(result.ok) << result.error;

  std::vector<std::uint64_t> seen;
  seen.reserve(3);

  EXPECT_EQ(result.value.Poll(2,
                              [&](const OrderFeedbackEvent& event) {
                                seen.push_back(event.local_order_id);
                              }),
            2U);
  EXPECT_EQ(result.value.consumed_count(), 2U);
  ASSERT_EQ(seen.size(), 2U);
  EXPECT_EQ(seen[0], LocalOrderIdCodec::Encode(2, 1));
  EXPECT_EQ(seen[1], LocalOrderIdCodec::Encode(2, 2));

  EXPECT_EQ(result.value.Poll(2,
                              [&](const OrderFeedbackEvent& event) {
                                seen.push_back(event.local_order_id);
                              }),
            1U);
  EXPECT_EQ(result.value.consumed_count(), 3U);
  ASSERT_EQ(seen.size(), 3U);
  EXPECT_EQ(seen[2], LocalOrderIdCodec::Encode(2, 3));
}

TEST(OrderFeedbackShmTest, ReaderPollZeroDoesNotConsume) {
  auto channel = MakeChannelForTest();
  ASSERT_TRUE(channel->lanes[7].queue.TryPush(MakeAcceptedEvent(7, 700)));

  auto result = OrderFeedbackShmReader::Claim(*channel, 7, 88);
  ASSERT_TRUE(result.ok) << result.error;

  bool called = false;
  EXPECT_EQ(
      result.value.Poll(0, [&](const OrderFeedbackEvent&) { called = true; }),
      0U);
  EXPECT_FALSE(called);
  EXPECT_EQ(result.value.consumed_count(), 0U);

  EXPECT_EQ(result.value.Poll(1,
                              [&](const OrderFeedbackEvent& event) {
                                called = true;
                                EXPECT_EQ(event.local_order_id,
                                          LocalOrderIdCodec::Encode(7, 700));
                              }),
            1U);
  EXPECT_TRUE(called);
  EXPECT_EQ(result.value.consumed_count(), 1U);
}

TEST(OrderFeedbackShmTest, ReaderPollDeliversGapEventAsNormalEvent) {
  auto channel = MakeChannelForTest();
  OrderFeedbackEvent gap{};
  gap.kind = OrderFeedbackKind::kGap;
  gap.gap_scope = OrderFeedbackGapScope::kGlobal;
  gap.gap_reason = OrderFeedbackGapReason::kProducerRestart;
  gap.gap_sequence = 12345;
  ASSERT_TRUE(channel->lanes[0].queue.TryPush(gap));

  auto result = OrderFeedbackShmReader::Claim(*channel, 0, 99);
  ASSERT_TRUE(result.ok) << result.error;

  bool saw_gap = false;
  EXPECT_EQ(result.value.Poll(
                1,
                [&](const OrderFeedbackEvent& event) {
                  saw_gap = true;
                  EXPECT_EQ(event.kind, OrderFeedbackKind::kGap);
                  EXPECT_EQ(event.gap_scope, OrderFeedbackGapScope::kGlobal);
                  EXPECT_EQ(event.gap_reason,
                            OrderFeedbackGapReason::kProducerRestart);
                  EXPECT_EQ(event.gap_sequence, 12345U);
                }),
            1U);

  EXPECT_TRUE(saw_gap);
  EXPECT_EQ(result.value.consumed_count(), 1U);
}

}  // namespace
}  // namespace aquila
