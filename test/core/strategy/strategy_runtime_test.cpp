#include "core/strategy/strategy_runtime.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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

struct RuntimeLoopState {
  bool order_ready{true};
  bool order_running{true};
  bool order_start_result{true};
  bool stop_immediately{false};
  int start_calls{0};
  int stop_calls{0};
  int order_response_poll_calls{0};
  int data_poll_calls{0};
  int on_start_calls{0};
  int on_idle_calls{0};
  int on_loop_calls{0};
  int on_stop_calls{0};
  int should_stop_calls{0};
  int book_ticker_calls{0};
  int response_calls{0};
  int feedback_calls{0};
  int stop_after_book_ticker_calls{0};
  int stop_after_response_calls{0};
  int stop_after_feedback_calls{0};
  int stop_after_idle_calls{0};
  std::int64_t last_ticker_id{0};
  OrderResponseKind last_response_kind{OrderResponseKind::kAck};
  std::vector<BookTicker> book_tickers;
  std::vector<OrderResponseEvent> order_responses;
  std::size_t next_book_ticker_index{0};
  std::size_t next_order_response_index{0};
};

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  explicit FakeOrderSession(RuntimeLoopState* loop_state = nullptr) noexcept
      : loop_state(loop_state) {}
  FakeOrderSession(FakeOrderSession&&) noexcept = default;
  FakeOrderSession& operator=(FakeOrderSession&&) noexcept = default;
  FakeOrderSession(const FakeOrderSession&) = delete;
  FakeOrderSession& operator=(const FakeOrderSession&) = delete;

  bool Start() noexcept {
    if (loop_state != nullptr) {
      ++loop_state->start_calls;
      return loop_state->order_start_result;
    }
    return true;
  }

  void Stop() noexcept {
    if (loop_state != nullptr) {
      ++loop_state->stop_calls;
    }
  }

  [[nodiscard]] bool Ready() const noexcept {
    return loop_state == nullptr || loop_state->order_ready;
  }

  [[nodiscard]] bool Running() const noexcept {
    return loop_state == nullptr || loop_state->order_running;
  }

  template <typename Handler>
  std::uint64_t PollOrderResponses(Handler& handler) noexcept {
    if (loop_state == nullptr) {
      return 0;
    }
    ++loop_state->order_response_poll_calls;
    if (loop_state->next_order_response_index >=
        loop_state->order_responses.size()) {
      return 0;
    }
    handler.OnOrderResponse(
        loop_state->order_responses[loop_state->next_order_response_index++]);
    return 1;
  }

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
  RuntimeLoopState* loop_state{nullptr};
};

struct RuntimeStrategyState {
  bool book_ticker_called{false};
  bool response_called{false};
  bool feedback_called{false};
  std::int64_t last_ticker_id{0};
  std::uint64_t placed_local_order_id{0};
  OrderStatus observed_response_status{OrderStatus::kCreated};
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

  void OnOrderResponse(const OrderResponseEvent& event,
                       ContextT& context) noexcept {
    state_->response_called = true;
    const StrategyOrder* order = context.FindOrder(event.local_order_id);
    if (order != nullptr) {
      state_->observed_response_status = order->status;
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

RuntimeLoopState* g_fake_data_reader_state = nullptr;

struct FakeDataReader {
  explicit FakeDataReader(config::DataReaderConfig) noexcept
      : state(g_fake_data_reader_state) {}

  template <typename Handler>
  std::uint64_t Poll(Handler& handler) noexcept {
    if (state == nullptr) {
      return 0;
    }
    ++state->data_poll_calls;
    if (state->next_book_ticker_index >= state->book_tickers.size()) {
      return 0;
    }
    handler.OnBookTicker(state->book_tickers[state->next_book_ticker_index++]);
    return 1;
  }

  RuntimeLoopState* state{nullptr};
};

struct LoopUserStrategy {
  using ContextT = StrategyContext<FakeOrderSession>;

  explicit LoopUserStrategy(RuntimeLoopState* state) noexcept : state_(state) {}

  void OnStart(ContextT&) noexcept {
    ++state_->on_start_calls;
  }

  void OnIdle(ContextT&) noexcept {
    ++state_->on_idle_calls;
  }

  void OnLoop(ContextT&) noexcept {
    ++state_->on_loop_calls;
  }

  void OnStop(ContextT&) noexcept {
    ++state_->on_stop_calls;
  }

  [[nodiscard]] bool ShouldStop() noexcept {
    ++state_->should_stop_calls;
    if (state_->stop_immediately) {
      return true;
    }
    if (state_->stop_after_book_ticker_calls > 0 &&
        state_->book_ticker_calls >= state_->stop_after_book_ticker_calls) {
      return true;
    }
    if (state_->stop_after_response_calls > 0 &&
        state_->response_calls >= state_->stop_after_response_calls) {
      return true;
    }
    if (state_->stop_after_feedback_calls > 0 &&
        state_->feedback_calls >= state_->stop_after_feedback_calls) {
      return true;
    }
    if (state_->stop_after_idle_calls > 0 &&
        state_->on_idle_calls >= state_->stop_after_idle_calls) {
      return true;
    }
    return false;
  }

  void OnBookTicker(const BookTicker& ticker, ContextT&) noexcept {
    ++state_->book_ticker_calls;
    state_->last_ticker_id = ticker.id;
  }

  void OnOrderResponse(const OrderResponseEvent& event, ContextT&) noexcept {
    ++state_->response_calls;
    state_->last_response_kind = event.kind;
  }

  void OnOrderFeedback(const OrderFeedbackEvent&, ContextT&) noexcept {
    ++state_->feedback_calls;
  }

 private:
  RuntimeLoopState* state_;
};

using LoopRuntime =
    StrategyRuntime<LoopUserStrategy, FakeOrderSession, FakeDataReader>;

struct ThrowingSessionUserStrategy {
  using ContextT = StrategyContext<ThrowingOrderSession>;

  explicit ThrowingSessionUserStrategy(RuntimeStrategyState*) noexcept {}

  void OnBookTicker(const BookTicker&, ContextT&) noexcept {}
  void OnOrderResponse(const OrderResponseEvent&, ContextT&) noexcept {}
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

config::DataReaderConfig MakeDataReaderConfig() {
  config::DataReaderConfig config;
  config.name = "fake_strategy_runtime_reader";
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

TEST(StrategyRuntimeTest,
     ResponseDispatchUpdatesOrderManagerBeforeUserStrategyHook) {
  RuntimeStrategyState state;
  auto runtime_result = Runtime::CreateForTest(
      MakeRuntimeConfig(), [] { return FakeOrderSession{}; }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);
  runtime_result.value->HandleBookTickerForTest(MakeBookTicker());
  ASSERT_NE(state.placed_local_order_id, 0U);

  runtime_result.value->HandleOrderResponseForTest(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = state.placed_local_order_id,
      .exchange_order_id = 36028827892199865U,
  });

  EXPECT_TRUE(state.response_called);
  EXPECT_EQ(state.observed_response_status, OrderStatus::kAccepted);
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

TEST(StrategyRuntimeTest,
     ProductionRunDispatchesDataReaderBookTickerWhenOrderSessionReady) {
  RuntimeLoopState state;
  state.book_tickers.push_back(MakeBookTicker());
  state.stop_after_book_ticker_calls = 1;
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;

  auto runtime_result = LoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  EXPECT_EQ(state.start_calls, 1);
  EXPECT_EQ(state.stop_calls, 1);
  EXPECT_EQ(state.on_start_calls, 1);
  EXPECT_EQ(state.on_stop_calls, 1);
  EXPECT_EQ(state.on_loop_calls, 1);
  EXPECT_EQ(state.data_poll_calls, 1);
  EXPECT_EQ(state.book_ticker_calls, 1);
  EXPECT_EQ(state.last_ticker_id, 42);
}

TEST(StrategyRuntimeTest, ProductionRunDispatchesOrderSessionResponses) {
  RuntimeLoopState state;
  state.order_ready = false;
  state.stop_after_response_calls = 1;
  state.order_responses.push_back(OrderResponseEvent{
      .kind = OrderResponseKind::kRejected,
      .local_order_id = 77,
      .exchange_order_id = 0,
  });
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;

  auto runtime_result = LoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  EXPECT_EQ(state.order_response_poll_calls, 1);
  EXPECT_EQ(state.response_calls, 1);
  EXPECT_EQ(state.last_response_kind, OrderResponseKind::kRejected);
  EXPECT_EQ(state.data_poll_calls, 0);
}

TEST(StrategyRuntimeTest, ProductionRunPollsOrderFeedbackReader) {
  OrderFeedbackShmConfig shm_config{
      .shm_name = "srt_fb_test",
      .channel_name = "srt_fb_ch",
      .create = true,
      .remove_existing = true,
  };
  auto manager_result = OrderFeedbackShmManager::Create(shm_config);
  ASSERT_TRUE(manager_result.ok) << manager_result.error;
  OrderFeedbackShmPublisher publisher(manager_result.value.channel());
  ASSERT_TRUE(publisher.PublishGlobalGap(
      OrderFeedbackGapReason::kSessionDisconnected, 123456));

  RuntimeLoopState state;
  state.order_ready = false;
  state.stop_after_feedback_calls = 1;
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = true;
  config.feedback.shm_name = shm_config.shm_name;
  config.feedback.channel_name = shm_config.channel_name;
  config.feedback.poll_budget = 4;
  config.feedback.force_claim = true;

  auto runtime_result = LoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  EXPECT_EQ(state.feedback_calls, 1);
  EXPECT_EQ(state.data_poll_calls, 0);
}

TEST(StrategyRuntimeTest, ProductionRunStopsWhenUserStrategyShouldStop) {
  RuntimeLoopState state;
  state.stop_immediately = true;
  state.book_tickers.push_back(MakeBookTicker());
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;

  auto runtime_result = LoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  EXPECT_GT(state.should_stop_calls, 0);
  EXPECT_EQ(state.data_poll_calls, 0);
  EXPECT_EQ(state.book_ticker_calls, 0);
  EXPECT_EQ(state.stop_calls, 1);
}

TEST(StrategyRuntimeTest,
     ProductionRunDoesNotPollDataReaderBeforeOrderSessionReady) {
  RuntimeLoopState state;
  state.order_ready = false;
  state.stop_after_idle_calls = 3;
  state.book_tickers.push_back(MakeBookTicker());
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;
  config.loop.idle_policy = config::StrategyLoopIdlePolicy::kYield;

  auto runtime_result = LoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  EXPECT_GE(state.on_idle_calls, 3);
  EXPECT_EQ(state.data_poll_calls, 0);
  EXPECT_EQ(state.book_ticker_calls, 0);
}

TEST(StrategyRuntimeTest, ProductionRunFailsWhenOrderSessionStopsRunning) {
  RuntimeLoopState state;
  state.order_running = false;
  state.book_tickers.push_back(MakeBookTicker());
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;

  auto runtime_result = LoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 1);

  EXPECT_EQ(state.start_calls, 1);
  EXPECT_EQ(state.stop_calls, 1);
  EXPECT_EQ(state.data_poll_calls, 0);
  EXPECT_EQ(state.book_ticker_calls, 0);
}

}  // namespace
}  // namespace aquila::strategy
