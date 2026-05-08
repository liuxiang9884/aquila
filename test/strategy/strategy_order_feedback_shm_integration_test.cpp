#include <cstdint>
#include <memory>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/strategy/order_manager.h"
#include "core/strategy/order_types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_feedback_shm.h"
#include "core/trading/order_id.h"

namespace aquila::strategy {
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

  SendResult PlaceOrder(Order& order) noexcept {
    ++place_calls;
    EXPECT_EQ(order.symbol, "BTC_USDT");
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(Order&) noexcept {
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

OrderCreateRequest MakeLimitRequest() noexcept {
  return OrderCreateRequest{.exchange = Exchange::kGate,
                            .symbol_id = 7,
                            .symbol = "BTC_USDT",
                            .side = OrderSide::kBuy,
                            .time_in_force = TimeInForce::kGoodTillCancel,
                            .quantity = 2,
                            .price_text = "81000",
                            .reduce_only = false};
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

}  // namespace
}  // namespace aquila::strategy
