#include "core/strategy/strategy_runtime.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/config/strategy_config.h"
#include "core/market_data/types.h"
#include "core/strategy/order_types.h"
#include "core/strategy/strategy_context.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_id.h"

namespace aquila::strategy {
namespace {

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  FakeOrderSession() = default;
  FakeOrderSession(FakeOrderSession&&) noexcept = default;
  FakeOrderSession& operator=(FakeOrderSession&&) noexcept = default;
  FakeOrderSession(const FakeOrderSession&) = delete;
  FakeOrderSession& operator=(const FakeOrderSession&) = delete;

  int place_calls{0};
  int cache_update_calls{0};
  std::uint64_t last_cache_local_order_id{0};
  std::uint64_t last_cache_exchange_order_id{0};

  SendResult PlaceOrder(StrategyOrder& order) noexcept {
    ++place_calls;
    last_place_local_order_id = order.local_order_id;
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(StrategyOrder& order) noexcept {
    last_cancel_local_order_id = order.local_order_id;
    return {.status = SendStatus::kOk};
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    ++cache_update_calls;
    last_cache_local_order_id = local_order_id;
    last_cache_exchange_order_id = exchange_order_id;
  }

  std::uint64_t last_place_local_order_id{0};
  std::uint64_t last_cancel_local_order_id{0};
};

struct RuntimeStrategyState {
  bool book_ticker_called{false};
  bool feedback_called{false};
  std::int64_t last_ticker_id{0};
  std::uint64_t placed_local_order_id{0};
  OrderStatus observed_feedback_status{OrderStatus::kCreated};
};

struct ThrowingOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  explicit ThrowingOrderSession(bool should_throw) {
    if (should_throw) {
      throw std::runtime_error("order session construction failed");
    }
  }

  ThrowingOrderSession(ThrowingOrderSession&&) noexcept = default;
  ThrowingOrderSession& operator=(ThrowingOrderSession&&) noexcept = default;
  ThrowingOrderSession(const ThrowingOrderSession&) = delete;
  ThrowingOrderSession& operator=(const ThrowingOrderSession&) = delete;

  SendResult PlaceOrder(StrategyOrder&) noexcept {
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(StrategyOrder&) noexcept {
    return {.status = SendStatus::kOk};
  }
};

struct FakeUserStrategy {
  using ContextT = StrategyContext<FakeOrderSession>;

  explicit FakeUserStrategy(RuntimeStrategyState* state) noexcept
      : state_(state) {}

  FakeUserStrategy(FakeUserStrategy&&) noexcept = default;
  FakeUserStrategy& operator=(FakeUserStrategy&&) noexcept = default;
  FakeUserStrategy(const FakeUserStrategy&) = delete;
  FakeUserStrategy& operator=(const FakeUserStrategy&) = delete;

  void OnBookTicker(const BookTicker& ticker, ContextT& context) noexcept {
    state_->book_ticker_called = true;
    state_->last_ticker_id = ticker.id;
    const OrderPlaceResult placed = context.PlaceLimitOrder(MakeLimitRequest());
    state_->placed_local_order_id = placed.local_order_id;
  }

  void OnOrderFeedback(const OrderFeedbackEvent& event,
                       ContextT& context) noexcept {
    state_->feedback_called = true;
    const StrategyOrder* order = context.FindOrder(event.local_order_id);
    if (order != nullptr) {
      state_->observed_feedback_status = order->status;
    }
  }

 private:
  static OrderCreateRequest MakeLimitRequest() noexcept {
    return OrderCreateRequest{.exchange = Exchange::kGate,
                              .symbol_id = 7,
                              .symbol = "BTC_USDT",
                              .side = OrderSide::kBuy,
                              .time_in_force = TimeInForce::kGoodTillCancel,
                              .quantity = 1,
                              .price_text = "81000",
                              .reduce_only = false};
  }

  RuntimeStrategyState* state_;
};

using Runtime = StrategyRuntime<FakeUserStrategy, FakeOrderSession>;

struct ThrowingSessionUserStrategy {
  using ContextT = StrategyContext<ThrowingOrderSession>;

  explicit ThrowingSessionUserStrategy(RuntimeStrategyState*) noexcept {}

  void OnBookTicker(const BookTicker&, ContextT&) noexcept {}
  void OnOrderFeedback(const OrderFeedbackEvent&, ContextT&) noexcept {}
};

using ThrowingSessionRuntime =
    StrategyRuntime<ThrowingSessionUserStrategy, ThrowingOrderSession>;

config::StrategyConfig MakeRuntimeConfig() {
  config::StrategyConfig config;
  config.name = "runtime_test";
  config.strategy_id = 4;
  config.order_capacity = 8;
  config.feedback.enabled = true;
  config.feedback.shm_name = "unused_runtime_test_feedback";
  config.feedback.channel_name = "unused_runtime_test_feedback_channel";
  return config;
}

BookTicker MakeBookTicker() noexcept {
  return BookTicker{.id = 42,
                    .symbol_id = 7,
                    .exchange = Exchange::kGate,
                    .exchange_ns = 1000,
                    .local_ns = 2000,
                    .bid_price = 80999.5,
                    .bid_volume = 3.0,
                    .ask_price = 81000.0,
                    .ask_volume = 4.0};
}

TEST(StrategyRuntimeTest, RuntimeIsNonCopyableAndNonMovable) {
  static_assert(!std::is_copy_constructible_v<Runtime>);
  static_assert(!std::is_copy_assignable_v<Runtime>);
  static_assert(!std::is_move_constructible_v<Runtime>);
  static_assert(!std::is_move_assignable_v<Runtime>);
}

TEST(StrategyRuntimeTest, DispatchesBookTickerToUserStrategy) {
  RuntimeStrategyState state;
  auto runtime_result = Runtime::CreateForTest(
      MakeRuntimeConfig(), [] { return FakeOrderSession{}; }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  runtime_result.value->HandleBookTickerForTest(MakeBookTicker());

  EXPECT_TRUE(state.book_ticker_called);
  EXPECT_EQ(state.last_ticker_id, 42);
  EXPECT_NE(state.placed_local_order_id, 0U);
  EXPECT_EQ(LocalOrderIdCodec::StrategyId(state.placed_local_order_id), 4);
}

TEST(StrategyRuntimeTest,
     FeedbackDispatchUpdatesOrderManagerBeforeUserStrategyHook) {
  RuntimeStrategyState state;
  auto runtime_result = Runtime::CreateForTest(
      MakeRuntimeConfig(), [] { return FakeOrderSession{}; }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);
  runtime_result.value->HandleBookTickerForTest(MakeBookTicker());
  ASSERT_NE(state.placed_local_order_id, 0U);

  runtime_result.value->HandleOrderFeedbackForTest(OrderFeedbackEvent{
      .kind = OrderFeedbackKind::kAccepted,
      .local_order_id = state.placed_local_order_id,
      .exchange_order_id = 36028827892199865U,
      .exchange_update_ns = 1234567890,
  });

  EXPECT_TRUE(state.feedback_called);
  EXPECT_EQ(state.observed_feedback_status, OrderStatus::kAccepted);
}

TEST(StrategyRuntimeTest, FeedbackDisabledDoesNotRequireFeedbackReader) {
  RuntimeStrategyState state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;
  config.feedback.shm_name.clear();
  config.feedback.channel_name.clear();

  auto runtime_result = Runtime::CreateForTest(
      std::move(config), [] { return FakeOrderSession{}; }, &state);

  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  EXPECT_NE(runtime_result.value, nullptr);
}

TEST(StrategyRuntimeTest, RejectsZeroOrderCapacity) {
  RuntimeStrategyState state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.order_capacity = 0;

  auto runtime_result = Runtime::CreateForTest(
      std::move(config), [] { return FakeOrderSession{}; }, &state);

  EXPECT_FALSE(runtime_result.ok);
  EXPECT_EQ(runtime_result.error, "strategy.order_capacity must be positive");
  EXPECT_EQ(runtime_result.value, nullptr);
}

TEST(StrategyRuntimeTest, OrderSessionConstructionFailureReturnsResultError) {
  RuntimeStrategyState state;

  auto runtime_result = ThrowingSessionRuntime::CreateForTest(
      MakeRuntimeConfig(), [] { return ThrowingOrderSession(true); }, &state);

  EXPECT_FALSE(runtime_result.ok);
  EXPECT_EQ(runtime_result.error, "order session construction failed");
  EXPECT_EQ(runtime_result.value, nullptr);
}

}  // namespace
}  // namespace aquila::strategy
