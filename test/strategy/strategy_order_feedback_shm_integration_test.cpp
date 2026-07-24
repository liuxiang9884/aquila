#include <cstdint>
#include <memory>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_feedback_shm.h"
#include "core/trading/order_id.h"
#include "core/trading/order_manager.h"
#include "core/trading/order_types.h"

namespace aquila::core {
namespace {

struct FakeGateway {
  using Order = StrategyOrder;

  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  int place_calls{0};
  int cache_update_calls{0};
  int cache_forget_calls{0};
  std::uint64_t last_cache_local_order_id{0};
  std::uint64_t last_cache_exchange_order_id{0};
  std::uint64_t last_forget_local_order_id{0};

  SendResult PlaceOrder(const OrderPlaceRequest& request) noexcept {
    ++place_calls;
    EXPECT_EQ(request.SymbolView(), "BTC_USDT");
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(const OrderCancelRequest&) noexcept {
    return {.status = SendStatus::kOk};
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    ++cache_update_calls;
    last_cache_local_order_id = local_order_id;
    last_cache_exchange_order_id = exchange_order_id;
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    ++cache_forget_calls;
    last_forget_local_order_id = local_order_id;
  }
};

std::unique_ptr<OrderFeedbackShmChannel> MakeChannel() {
  auto channel = std::make_unique<OrderFeedbackShmChannel>();
  order_feedback_shm_detail::InitializeLaneHeaders(*channel);
  return channel;
}

OrderPlaceRequest MakeLimitRequest(std::uint64_t group_id = 0) noexcept {
  OrderPlaceRequest request{
      .group_id = group_id,
      .price = 81000,
      .quantity = 2,
      .symbol_id = 7,
      .exchange = Exchange::kGate,
      .side = OrderSide::kBuy,
      .time_in_force = TimeInForce::kGoodTillCancel,
      .price_decimal_places = 0,
      .quantity_decimal_places = 0,
      .reduce_only = false,
  };
  SetOrderSymbol(&request, "BTC_USDT");
  return request;
}

TEST(OrderManagerFeedbackShmIntegrationTest,
     ReaderPollAppliesAcceptedAndFilledFeedback) {
  auto channel = MakeChannel();
  OrderFeedbackShmPublisher publisher(*channel);
  auto reader_result = OrderFeedbackShmReader::Claim(*channel, 3, 101, true);
  ASSERT_TRUE(reader_result.ok) << reader_result.error;

  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8, 3);
  const OrderPlaceResult placed =
      order_manager.PlaceLimitOrder(MakeLimitRequest());
  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(LocalOrderIdCodec::StrategyId(placed.local_order_id), 3);

  ASSERT_TRUE(publisher.Publish(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kAccepted,
      .local_order_id = placed.local_order_id,
      .exchange_order_id = 36028827892199865U,
      .exchange_update_ns = 1000,
      .local_receive_ns = 1100,
  }));
  EXPECT_EQ(reader_result.value.Poll(8,
                                     [&](const OrderFeedbackEvent& event) {
                                       order_manager.OnOrderFeedback(event);
                                     }),
            1U);

  const StrategyOrder* accepted_order =
      order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(accepted_order, nullptr);
  EXPECT_EQ(accepted_order->status, OrderStatus::kAccepted);
  EXPECT_EQ(accepted_order->exchange_order_id, 36028827892199865U);
  EXPECT_EQ(gateway.cache_update_calls, 1);
  EXPECT_EQ(gateway.last_cache_local_order_id, placed.local_order_id);
  EXPECT_EQ(gateway.last_cache_exchange_order_id, 36028827892199865U);

  ASSERT_TRUE(publisher.Publish(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kFilled,
      .local_order_id = placed.local_order_id,
      .cumulative_filled_quantity = 2,
      .left_quantity = 0,
      .fill_price = 81000.5,
      .role = OrderRole::kMaker,
      .exchange_update_ns = 2000,
      .local_receive_ns = 2100,
  }));
  EXPECT_EQ(reader_result.value.Poll(8,
                                     [&](const OrderFeedbackEvent& event) {
                                       order_manager.OnOrderFeedback(event);
                                     }),
            1U);

  const StrategyOrder* filled_order =
      order_manager.FindOrder(placed.local_order_id);
  ASSERT_NE(filled_order, nullptr);
  EXPECT_EQ(filled_order->status, OrderStatus::kFilled);
  EXPECT_TRUE(filled_order->is_finished);
  EXPECT_EQ(filled_order->cumulative_filled_quantity, 2);
  EXPECT_DOUBLE_EQ(filled_order->AverageFillPrice(), 81000.5);
  EXPECT_EQ(filled_order->role, OrderRole::kMaker);
  EXPECT_EQ(gateway.cache_forget_calls, 1);
  EXPECT_EQ(gateway.last_forget_local_order_id, placed.local_order_id);
}

TEST(OrderManagerFeedbackShmIntegrationTest,
     ReverseFeedbackKeepsLocalGroupMetadataIsolated) {
  auto channel = MakeChannel();
  OrderFeedbackShmPublisher publisher(*channel);
  auto reader_result = OrderFeedbackShmReader::Claim(*channel, 3, 102, true);
  ASSERT_TRUE(reader_result.ok) << reader_result.error;

  FakeGateway gateway;
  OrderManager<FakeGateway> order_manager(gateway, 8, 3);
  const OrderPlaceResult group_a = order_manager.PlaceLimitOrder(
      MakeLimitRequest(701), OrderLocalMetadata{.group_index = 0});
  const OrderPlaceResult group_b = order_manager.PlaceLimitOrder(
      MakeLimitRequest(702), OrderLocalMetadata{.group_index = 1});
  ASSERT_EQ(group_a.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(group_b.status, OrderPlaceStatus::kOk);
  ASSERT_NE(group_a.local_order_id, group_b.local_order_id);

  ASSERT_TRUE(publisher.Publish(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kFilled,
      .local_order_id = group_b.local_order_id,
      .cumulative_filled_quantity = 2,
      .left_quantity = 0,
      .fill_price = 81002,
      .exchange_update_ns = 2002,
      .local_receive_ns = 2102,
  }));
  ASSERT_TRUE(publisher.Publish(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kFilled,
      .local_order_id = group_a.local_order_id,
      .cumulative_filled_quantity = 1,
      .left_quantity = 0,
      .fill_price = 81001,
      .exchange_update_ns = 2001,
      .local_receive_ns = 2101,
  }));
  EXPECT_EQ(reader_result.value.Poll(8,
                                     [&](const OrderFeedbackEvent& event) {
                                       order_manager.OnOrderFeedback(event);
                                     }),
            2U);

  const StrategyOrder* order_a =
      order_manager.FindOrder(group_a.local_order_id);
  const StrategyOrder* order_b =
      order_manager.FindOrder(group_b.local_order_id);
  ASSERT_NE(order_a, nullptr);
  ASSERT_NE(order_b, nullptr);
  EXPECT_EQ(order_a->place_request.group_id, 701U);
  EXPECT_EQ(order_a->group_index, 0U);
  EXPECT_EQ(order_a->cumulative_filled_quantity, 1);
  EXPECT_DOUBLE_EQ(order_a->AverageFillPrice(), 81001);
  EXPECT_EQ(order_b->place_request.group_id, 702U);
  EXPECT_EQ(order_b->group_index, 1U);
  EXPECT_EQ(order_b->cumulative_filled_quantity, 2);
  EXPECT_DOUBLE_EQ(order_b->AverageFillPrice(), 81002);
}

}  // namespace
}  // namespace aquila::core
