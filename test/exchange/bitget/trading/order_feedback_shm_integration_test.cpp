#include <cstdint>
#include <memory>
#include <string>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/trading/order_feedback_event.h"
#include "core/trading/order_feedback_shm.h"
#include "core/trading/order_id.h"
#include "exchange/bitget/trading/order_feedback_parser.h"
#include <simdjson.h>

namespace aquila::bitget {
namespace {

std::unique_ptr<OrderFeedbackShmChannel> MakeChannel() {
  auto channel = std::make_unique<OrderFeedbackShmChannel>();
  order_feedback_shm_detail::InitializeLaneHeaders(*channel);
  return channel;
}

OrderFeedbackEvent MakeAccepted(std::uint8_t strategy_id,
                                std::uint64_t strategy_order_id) {
  OrderFeedbackEvent event{};
  event.kind = OrderFeedbackKind::kAccepted;
  event.local_order_id =
      LocalOrderIdCodec::Encode(strategy_id, strategy_order_id);
  event.exchange_order_id = 9000 + strategy_order_id;
  event.left_quantity = 1.0;
  event.local_receive_ns = 123;
  return event;
}

TEST(BitgetOrderFeedbackShmIntegrationTest, ParserRoutesToStrategyLane) {
  auto channel = MakeChannel();
  OrderFeedbackShmPublisher publisher(*channel);
  constexpr std::uint8_t kStrategyId = 3;
  const std::uint64_t local_order_id =
      LocalOrderIdCodec::Encode(kStrategyId, 42);
  const std::string payload = fmt::format(
      R"({{"action":"snapshot","arg":{{"instType":"UTA","topic":"order"}},"data":[{{"category":"usdt-futures","orderId":"9988","clientOid":"a-{}","qty":"1.5","holdMode":"one_way_mode","marginMode":"crossed","cumExecQty":"0","avgPrice":"0","orderStatus":"new","updatedTime":"1750034397076"}}]}})",
      local_order_id);
  simdjson::ondemand::parser parser;
  OrderFeedbackParserStats stats;

  const OrderFeedbackParseResult result = ParseBitgetOrderFeedbackMessage(
      payload, 0, 1'750'034'397'080'123'456LL, parser, stats,
      [&publisher](const OrderFeedbackEvent& event) noexcept {
        return publisher.Publish(event);
      });

  ASSERT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  for (std::uint8_t lane_index = 0; lane_index < kMaxOrderFeedbackStrategies;
       ++lane_index) {
    OrderFeedbackEvent event{};
    if (lane_index == kStrategyId) {
      ASSERT_TRUE(channel->lanes[lane_index].queue.TryPop(event));
      EXPECT_EQ(event.local_order_id, local_order_id);
      EXPECT_EQ(event.kind, OrderFeedbackKind::kAccepted);
    } else {
      EXPECT_FALSE(channel->lanes[lane_index].queue.TryPop(event));
    }
  }
}

TEST(BitgetOrderFeedbackShmIntegrationTest,
     GlobalContinuityBroadcastsSameSequenceToEveryLane) {
  auto channel = MakeChannel();
  OrderFeedbackShmPublisher publisher(*channel);

  ASSERT_TRUE(publisher.PublishGlobalContinuityLost(
      OrderFeedbackContinuityReason::kDecodeUnrecoverable, 456));

  std::uint64_t sequence = 0;
  for (std::uint8_t lane_index = 0; lane_index < kMaxOrderFeedbackStrategies;
       ++lane_index) {
    OrderFeedbackEvent event{};
    ASSERT_TRUE(channel->lanes[lane_index].queue.TryPop(event));
    EXPECT_EQ(event.kind, OrderFeedbackKind::kContinuityLost);
    EXPECT_EQ(event.continuity_scope, OrderFeedbackContinuityScope::kGlobal);
    EXPECT_EQ(event.continuity_reason,
              OrderFeedbackContinuityReason::kDecodeUnrecoverable);
    if (lane_index == 0) {
      sequence = event.continuity_sequence;
      EXPECT_NE(sequence, 0U);
    } else {
      EXPECT_EQ(event.continuity_sequence, sequence);
    }
  }
}

TEST(BitgetOrderFeedbackShmIntegrationTest,
     QueueFullPublishesPendingLaneContinuityAfterDrain) {
  auto channel = MakeChannel();
  OrderFeedbackShmPublisher publisher(*channel);
  constexpr std::uint8_t kStrategyId = 2;
  OrderFeedbackLane& lane = channel->lanes[kStrategyId];

  std::uint64_t strategy_order_id = 1;
  while (lane.queue.TryPush(MakeAccepted(kStrategyId, strategy_order_id))) {
    ++strategy_order_id;
  }
  EXPECT_FALSE(publisher.Publish(MakeAccepted(kStrategyId, strategy_order_id)));
  EXPECT_EQ(lane.header.queue_full_count.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(lane.header.dropped_count.load(std::memory_order_relaxed), 1U);

  OrderFeedbackEvent event{};
  ASSERT_TRUE(lane.queue.TryPop(event));
  EXPECT_EQ(publisher.FlushPendingContinuityLostEvents(), 1U);

  bool found_continuity = false;
  while (lane.queue.TryPop(event)) {
    if (event.kind == OrderFeedbackKind::kContinuityLost) {
      found_continuity = true;
      EXPECT_EQ(event.continuity_scope, OrderFeedbackContinuityScope::kLane);
      EXPECT_EQ(event.continuity_reason,
                OrderFeedbackContinuityReason::kLaneQueueFull);
    }
  }
  EXPECT_TRUE(found_continuity);
}

}  // namespace
}  // namespace aquila::bitget
