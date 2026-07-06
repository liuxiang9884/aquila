#include "core/trading/trading_runtime.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/config/strategy_config.h"
#include "core/market_data/data_reader_concepts.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_id.h"
#include "core/trading/order_types.h"
#include "core/trading/strategy_context.h"
#include "core/websocket/websocket_client.h"

namespace aquila::core {
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
  int data_drain_calls{0};
  int on_start_calls{0};
  int on_idle_calls{0};
  int on_loop_calls{0};
  int on_stop_calls{0};
  int should_stop_calls{0};
  int book_ticker_calls{0};
  int response_calls{0};
  int feedback_calls{0};
  int bind_runtime_calls{0};
  int set_runtime_hook_calls{0};
  int runtime_hook_calls{0};
  int stop_after_book_ticker_calls{0};
  int stop_after_response_calls{0};
  int stop_after_feedback_calls{0};
  int stop_after_idle_calls{0};
  std::int64_t last_ticker_id{0};
  std::uint64_t last_data_drain_budget{0};
  std::uint64_t placed_local_order_id{0};
  OrderStatus observed_response_status{OrderStatus::kCreated};
  bool hook_stop_requested{false};
  bool emit_start_runtime_response{true};
  bool place_order_on_start{false};
  OrderResponseKind last_response_kind{OrderResponseKind::kAck};
  std::vector<BookTicker> book_tickers;
  std::vector<std::int64_t> handled_book_ticker_ids;
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

struct FakeHookOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  using RuntimeResponseCallback = void (*)(void*,
                                           const OrderResponseEvent&) noexcept;

  explicit FakeHookOrderSession(RuntimeLoopState* loop_state = nullptr) noexcept
      : loop_state(loop_state) {}
  FakeHookOrderSession(FakeHookOrderSession&&) noexcept = default;
  FakeHookOrderSession& operator=(FakeHookOrderSession&&) noexcept = default;
  FakeHookOrderSession(const FakeHookOrderSession&) = delete;
  FakeHookOrderSession& operator=(const FakeHookOrderSession&) = delete;

  template <typename RuntimeT>
  void BindRuntime(RuntimeT& runtime) noexcept {
    if (loop_state != nullptr) {
      ++loop_state->bind_runtime_calls;
    }
    bound_runtime = &runtime;
    runtime_response_callback = [](void* context,
                                   const OrderResponseEvent& event) noexcept {
      static_cast<RuntimeT*>(context)->OnOrderResponse(event);
    };
  }

  void SetRuntimeHook(void* context, websocket::RuntimeHook handler) noexcept {
    runtime_hook_context = context;
    runtime_hook_handler = handler;
    if (loop_state != nullptr) {
      ++loop_state->set_runtime_hook_calls;
    }
  }

  bool Start() noexcept {
    if (loop_state != nullptr) {
      ++loop_state->start_calls;
      loop_state->hook_stop_requested = false;
      if (!loop_state->order_start_result) {
        return false;
      }
    }
    if (runtime_response_callback != nullptr &&
        (loop_state == nullptr || loop_state->emit_start_runtime_response)) {
      runtime_response_callback(bound_runtime,
                                OrderResponseEvent{
                                    .kind = OrderResponseKind::kRejected,
                                    .local_order_id = 77,
                                    .exchange_order_id = 0,
                                });
    }
    while (loop_state != nullptr && !loop_state->hook_stop_requested &&
           runtime_hook_handler != nullptr &&
           loop_state->runtime_hook_calls < 8) {
      ++loop_state->runtime_hook_calls;
      runtime_hook_handler(runtime_hook_context);
    }
    return loop_state == nullptr || loop_state->order_start_result;
  }

  void Stop() noexcept {
    if (loop_state != nullptr) {
      ++loop_state->stop_calls;
      loop_state->hook_stop_requested = true;
    }
  }

  [[nodiscard]] bool Ready() const noexcept {
    return loop_state == nullptr || loop_state->order_ready;
  }

  [[nodiscard]] bool Running() const noexcept {
    return loop_state == nullptr || !loop_state->hook_stop_requested;
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

  SendResult PlaceOrder(StrategyOrder& order) noexcept {
    last_place_local_order_id = order.local_order_id;
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(StrategyOrder& order) noexcept {
    last_cancel_local_order_id = order.local_order_id;
    return {.status = SendStatus::kOk};
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    last_cache_local_order_id = local_order_id;
    last_cache_exchange_order_id = exchange_order_id;
  }

  RuntimeLoopState* loop_state{nullptr};
  void* bound_runtime{nullptr};
  RuntimeResponseCallback runtime_response_callback{nullptr};
  void* runtime_hook_context{nullptr};
  websocket::RuntimeHook runtime_hook_handler{nullptr};
  std::uint64_t last_place_local_order_id{0};
  std::uint64_t last_cancel_local_order_id{0};
  std::uint64_t last_cache_local_order_id{0};
  std::uint64_t last_cache_exchange_order_id{0};
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

struct FakeStrategy {
  using ContextT = StrategyContext<FakeOrderSession>;

  explicit FakeStrategy(RuntimeStrategyState* state) noexcept : state_(state) {}

  FakeStrategy(FakeStrategy&&) noexcept = default;
  FakeStrategy& operator=(FakeStrategy&&) noexcept = default;
  FakeStrategy(const FakeStrategy&) = delete;
  FakeStrategy& operator=(const FakeStrategy&) = delete;

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
                              .quantity_text = "1",
                              .price_text = "81000",
                              .reduce_only = false};
  }

  RuntimeStrategyState* state_;
};

using Runtime = TradingRuntime<FakeStrategy, FakeOrderSession>;

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

struct FakeDrainDataReader {
  explicit FakeDrainDataReader(config::DataReaderConfig) noexcept
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

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) noexcept {
    if (state == nullptr) {
      return 0;
    }
    ++state->data_drain_calls;
    state->last_data_drain_budget = max_events;
    std::uint64_t handled = 0;
    while (handled < max_events &&
           state->next_book_ticker_index < state->book_tickers.size()) {
      handler.OnBookTicker(
          state->book_tickers[state->next_book_ticker_index++]);
      ++handled;
    }
    return handled;
  }

  RuntimeLoopState* state{nullptr};
};

struct FakeFiniteDrainDataReader {
  static constexpr bool kFiniteDataReader = true;

  explicit FakeFiniteDrainDataReader(config::DataReaderConfig) noexcept
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

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) noexcept {
    if (state == nullptr) {
      return 0;
    }
    ++state->data_drain_calls;
    state->last_data_drain_budget = max_events;
    std::uint64_t handled = 0;
    while (handled < max_events &&
           state->next_book_ticker_index < state->book_tickers.size()) {
      handler.OnBookTicker(
          state->book_tickers[state->next_book_ticker_index++]);
      ++handled;
    }
    return handled;
  }

  [[nodiscard]] bool finished() const noexcept {
    return state == nullptr ||
           state->next_book_ticker_index >= state->book_tickers.size();
  }

  RuntimeLoopState* state{nullptr};
};

static_assert(
    !market_data::FiniteDataReader<FakeDrainDataReader>,
    "live-like readers can expose Drain helpers without replay EOF semantics");
static_assert(market_data::FiniteDataReader<FakeFiniteDrainDataReader>);

struct LoopStrategy {
  using ContextT = StrategyContext<FakeOrderSession>;

  explicit LoopStrategy(RuntimeLoopState* state) noexcept : state_(state) {}

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
    state_->handled_book_ticker_ids.push_back(ticker.id);
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
    TradingRuntime<LoopStrategy, FakeOrderSession, FakeDataReader>;
using DrainLoopRuntime =
    TradingRuntime<LoopStrategy, FakeOrderSession, FakeDrainDataReader>;
using FiniteDrainLoopRuntime =
    TradingRuntime<LoopStrategy, FakeOrderSession, FakeFiniteDrainDataReader>;
using DiagnosticLoopRuntime =
    TradingRuntime<LoopStrategy, FakeOrderSession, FakeDataReader,
                   TradingRuntimeDiagnostics>;
using DiagnosticFiniteDrainLoopRuntime =
    TradingRuntime<LoopStrategy, FakeOrderSession, FakeFiniteDrainDataReader,
                   TradingRuntimeDiagnostics>;

struct HookLoopStrategy {
  using ContextT = StrategyContext<FakeHookOrderSession>;

  explicit HookLoopStrategy(RuntimeLoopState* state) noexcept : state_(state) {}

  void OnStart(ContextT& context) noexcept {
    ++state_->on_start_calls;
    if (!state_->place_order_on_start) {
      return;
    }
    const OrderPlaceResult placed = context.PlaceLimitOrder(MakeLimitRequest());
    state_->placed_local_order_id = placed.local_order_id;
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
    return state_->stop_immediately;
  }

  void OnBookTicker(const BookTicker& ticker, ContextT&) noexcept {
    ++state_->book_ticker_calls;
    state_->last_ticker_id = ticker.id;
  }

  void OnOrderResponse(const OrderResponseEvent& event,
                       ContextT& context) noexcept {
    ++state_->response_calls;
    state_->last_response_kind = event.kind;
    const StrategyOrder* order = context.FindOrder(event.local_order_id);
    if (order != nullptr) {
      state_->observed_response_status = order->status;
    }
  }

  void OnOrderFeedback(const OrderFeedbackEvent&, ContextT&) noexcept {
    ++state_->feedback_calls;
  }

 private:
  static OrderCreateRequest MakeLimitRequest() noexcept {
    return OrderCreateRequest{.exchange = Exchange::kGate,
                              .symbol_id = 7,
                              .symbol = "BTC_USDT",
                              .side = OrderSide::kBuy,
                              .time_in_force = TimeInForce::kGoodTillCancel,
                              .quantity = 1,
                              .quantity_text = "1",
                              .price_text = "81000",
                              .reduce_only = false};
  }

  RuntimeLoopState* state_;
};

using HookLoopRuntime =
    TradingRuntime<HookLoopStrategy, FakeHookOrderSession, FakeDataReader>;

struct ThrowingSessionStrategy {
  using ContextT = StrategyContext<ThrowingOrderSession>;

  explicit ThrowingSessionStrategy(RuntimeStrategyState*) noexcept {}

  void OnBookTicker(const BookTicker&, ContextT&) noexcept {}
  void OnOrderResponse(const OrderResponseEvent&, ContextT&) noexcept {}
  void OnOrderFeedback(const OrderFeedbackEvent&, ContextT&) noexcept {}
};

using ThrowingSessionRuntime =
    TradingRuntime<ThrowingSessionStrategy, ThrowingOrderSession>;

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
  config.name = "fake_trading_runtime_reader";
  return config;
}

config::DataReaderConfig MakeTradeDataReaderConfig() {
  config::DataReaderConfig config = MakeDataReaderConfig();
  config.sources.push_back(config::DataReaderSourceConfig{
      .name = "gate_trade",
      .type = config::DataReaderSourceType::kShm,
      .exchange = Exchange::kGate,
      .feed = config::DataReaderFeed::kTrade,
      .shm_name = "unused_market_data",
      .channel_name = "trade_channel",
      .start_position = config::DataReaderStartPosition::kLatest,
      .read_mode = config::DataReaderReadMode::kDrain,
      .required = true,
  });
  return config;
}

config::DataReaderConfig MakeBinaryTradeDataReaderConfig() {
  config::DataReaderConfig config = MakeDataReaderConfig();
  config.sources.push_back(config::DataReaderSourceConfig{
      .name = "binary_trade",
      .type = config::DataReaderSourceType::kBinaryFile,
      .exchange = Exchange::kGate,
      .feed = config::DataReaderFeed::kTrade,
      .shm_name = {},
      .channel_name = {},
      .files = {std::filesystem::path{"/home/liuxiang/tmp/trade.bin"}},
      .start_position = config::DataReaderStartPosition::kEarliestVisible,
      .read_mode = config::DataReaderReadMode::kDrain,
      .required = true,
  });
  return config;
}

BookTicker MakeBookTicker(std::int64_t id = 42) noexcept {
  return BookTicker{.id = id,
                    .symbol_id = 7,
                    .exchange = Exchange::kGate,
                    .exchange_ns = 1000,
                    .local_ns = 2000,
                    .bid_price = 80999.5,
                    .bid_volume = 3.0,
                    .ask_price = 81000.0,
                    .ask_volume = 4.0};
}

TEST(TradingRuntimeTest, RuntimeIsNonCopyableAndNonMovable) {
  static_assert(!std::is_copy_constructible_v<Runtime>);
  static_assert(!std::is_copy_assignable_v<Runtime>);
  static_assert(!std::is_move_constructible_v<Runtime>);
  static_assert(!std::is_move_assignable_v<Runtime>);
}

TEST(TradingRuntimeTest, DispatchesBookTickerToStrategy) {
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

TEST(TradingRuntimeTest,
     FeedbackDispatchUpdatesOrderManagerBeforeStrategyHook) {
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

TEST(TradingRuntimeTest,
     ResponseDispatchUpdatesOrderManagerBeforeStrategyHook) {
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

TEST(TradingRuntimeTest, FeedbackDisabledDoesNotRequireFeedbackReader) {
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

TEST(TradingRuntimeTest, RejectsZeroOrderCapacity) {
  RuntimeStrategyState state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.order_capacity = 0;

  auto runtime_result = Runtime::CreateForTest(
      std::move(config), [] { return FakeOrderSession{}; }, &state);

  EXPECT_FALSE(runtime_result.ok);
  EXPECT_EQ(runtime_result.error, "strategy.order_capacity must be positive");
  EXPECT_EQ(runtime_result.value, nullptr);
}

TEST(TradingRuntimeTest, OrderSessionConstructionFailureReturnsResultError) {
  RuntimeStrategyState state;

  auto runtime_result = ThrowingSessionRuntime::CreateForTest(
      MakeRuntimeConfig(), [] { return ThrowingOrderSession(true); }, &state);

  EXPECT_FALSE(runtime_result.ok);
  EXPECT_EQ(runtime_result.error, "order session construction failed");
  EXPECT_EQ(runtime_result.value, nullptr);
}

TEST(TradingRuntimeTest,
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

TEST(TradingRuntimeTest,
     RejectsTradeDataReaderSourceWhenStrategyDoesNotHandleTrades) {
  RuntimeLoopState state;
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;

  auto runtime_result = LoopRuntime::Create(
      std::move(config), MakeTradeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);

  EXPECT_FALSE(runtime_result.ok);
  EXPECT_NE(runtime_result.error.find("OnTrade"), std::string::npos);
  EXPECT_EQ(runtime_result.value, nullptr);
}

TEST(TradingRuntimeTest,
     RejectsBinaryTradeDataReaderSourceWhenStrategyDoesNotHandleTrades) {
  RuntimeLoopState state;
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;

  auto runtime_result = LoopRuntime::Create(
      std::move(config), MakeBinaryTradeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);

  EXPECT_FALSE(runtime_result.ok);
  EXPECT_NE(runtime_result.error.find("OnTrade"), std::string::npos);
  EXPECT_EQ(runtime_result.value, nullptr);
}

TEST(TradingRuntimeTest, ProductionRunPollsLiveLikeDrainCapableDataReader) {
  RuntimeLoopState state;
  state.book_tickers.push_back(MakeBookTicker(101));
  state.book_tickers.push_back(MakeBookTicker(102));
  state.stop_after_book_ticker_calls = 1;
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;
  config::DataReaderConfig data_reader_config = MakeDataReaderConfig();
  data_reader_config.max_events_per_drain = 2;

  auto runtime_result = DrainLoopRuntime::Create(
      std::move(config), std::move(data_reader_config),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  EXPECT_EQ(state.data_poll_calls, 1);
  EXPECT_EQ(state.data_drain_calls, 0);
  EXPECT_EQ(state.book_ticker_calls, 1);
  ASSERT_EQ(state.handled_book_ticker_ids.size(), 1U);
  EXPECT_EQ(state.handled_book_ticker_ids[0], 101);
  EXPECT_EQ(state.on_loop_calls, 1);
}

TEST(TradingRuntimeTest,
     ProductionRunDrainsFiniteDataReaderWithConfiguredEventBudget) {
  RuntimeLoopState state;
  state.book_tickers.push_back(MakeBookTicker(101));
  state.book_tickers.push_back(MakeBookTicker(102));
  state.book_tickers.push_back(MakeBookTicker(103));
  state.stop_after_book_ticker_calls = 2;
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;
  config::DataReaderConfig data_reader_config = MakeDataReaderConfig();
  data_reader_config.max_events_per_drain = 2;

  auto runtime_result = FiniteDrainLoopRuntime::Create(
      std::move(config), std::move(data_reader_config),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  EXPECT_EQ(state.data_drain_calls, 1);
  EXPECT_EQ(state.data_poll_calls, 0);
  EXPECT_EQ(state.last_data_drain_budget, 2U);
  EXPECT_EQ(state.book_ticker_calls, 2);
  ASSERT_EQ(state.handled_book_ticker_ids.size(), 2U);
  EXPECT_EQ(state.handled_book_ticker_ids[0], 101);
  EXPECT_EQ(state.handled_book_ticker_ids[1], 102);
  EXPECT_EQ(state.on_loop_calls, 1);
}

TEST(TradingRuntimeTest, DiagnosticsRecordLivePollAndIdleLoop) {
  RuntimeLoopState state;
  state.stop_after_idle_calls = 1;
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;

  auto runtime_result = DiagnosticLoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  const TradingRuntimeLoopStats& stats =
      runtime_result.value->diagnostics().stats();
  EXPECT_EQ(stats.loop_iterations, 1U);
  EXPECT_EQ(stats.idle_iterations, 1U);
  EXPECT_EQ(stats.order_response_poll_calls, 1U);
  EXPECT_EQ(stats.order_response_empty_polls, 1U);
  EXPECT_EQ(stats.order_response_events, 0U);
  EXPECT_EQ(stats.order_feedback_poll_calls, 0U);
  EXPECT_EQ(stats.order_feedback_empty_polls, 0U);
  EXPECT_EQ(stats.order_feedback_events, 0U);
  EXPECT_EQ(stats.data_reader_poll_calls, 1U);
  EXPECT_EQ(stats.data_reader_drain_calls, 0U);
  EXPECT_EQ(stats.data_reader_empty_polls, 1U);
  EXPECT_EQ(stats.data_reader_events, 0U);
}

TEST(TradingRuntimeTest, DiagnosticsRecordFiniteDrainBudgetAndEvents) {
  RuntimeLoopState state;
  state.book_tickers.push_back(MakeBookTicker(101));
  state.book_tickers.push_back(MakeBookTicker(102));
  state.stop_after_book_ticker_calls = 2;
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;
  config::DataReaderConfig data_reader_config = MakeDataReaderConfig();
  data_reader_config.max_events_per_drain = 2;

  auto runtime_result = DiagnosticFiniteDrainLoopRuntime::Create(
      std::move(config), std::move(data_reader_config),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  const TradingRuntimeLoopStats& stats =
      runtime_result.value->diagnostics().stats();
  EXPECT_EQ(stats.loop_iterations, 1U);
  EXPECT_EQ(stats.idle_iterations, 0U);
  EXPECT_EQ(stats.order_response_poll_calls, 1U);
  EXPECT_EQ(stats.order_response_empty_polls, 1U);
  EXPECT_EQ(stats.order_response_events, 0U);
  EXPECT_EQ(stats.data_reader_poll_calls, 0U);
  EXPECT_EQ(stats.data_reader_drain_calls, 1U);
  EXPECT_EQ(stats.data_reader_empty_polls, 0U);
  EXPECT_EQ(stats.data_reader_events, 2U);
}

TEST(TradingRuntimeTest, ProductionRunDispatchesOrderSessionResponses) {
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

TEST(TradingRuntimeTest,
     HookModeBindsRuntimeAndDrivesPollingFromOrderSessionHook) {
  RuntimeLoopState state;
  state.book_tickers.push_back(MakeBookTicker());
  state.order_responses.push_back(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = 78,
      .exchange_order_id = 36028827892199865U,
  });
  state.stop_after_book_ticker_calls = 1;
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;

  auto runtime_result = HookLoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeHookOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  EXPECT_EQ(state.bind_runtime_calls, 1);
  EXPECT_EQ(state.set_runtime_hook_calls, 1);
  EXPECT_EQ(state.start_calls, 1);
  EXPECT_EQ(state.stop_calls, 1);
  EXPECT_EQ(state.runtime_hook_calls, 1);
  EXPECT_EQ(state.order_response_poll_calls, 1);
  EXPECT_EQ(state.data_poll_calls, 1);
  EXPECT_EQ(state.book_ticker_calls, 1);
  EXPECT_EQ(state.last_ticker_id, 42);
  EXPECT_EQ(state.on_start_calls, 1);
  EXPECT_EQ(state.on_loop_calls, 1);
  EXPECT_EQ(state.on_stop_calls, 1);
  EXPECT_GT(state.should_stop_calls, 0);
  EXPECT_EQ(state.response_calls, 2);
  EXPECT_EQ(state.last_response_kind, OrderResponseKind::kAccepted);
}

TEST(
    TradingRuntimeTest,
    HookModeDoesNotPollDataReaderBeforeOrderSessionReadyAndStillDrainsResponses) {
  OrderFeedbackShmConfig shm_config{
      .shm_name = "srt_hook_ready_fb_test",
      .channel_name = "srt_hook_ready_fb_ch",
      .create = true,
      .remove_existing = true,
  };
  auto manager_result = OrderFeedbackShmManager::Create(shm_config);
  ASSERT_TRUE(manager_result.ok) << manager_result.error;
  OrderFeedbackShmPublisher publisher(manager_result.value.channel());
  ASSERT_TRUE(publisher.PublishGlobalContinuityLost(
      OrderFeedbackContinuityReason::kSessionDisconnected, 123456));

  RuntimeLoopState state;
  state.order_ready = false;
  state.emit_start_runtime_response = false;
  // Seed an existing order for response-order assertions; this is not a
  // ready-gate order entry policy assertion.
  state.place_order_on_start = true;
  state.stop_after_feedback_calls = 1;
  state.book_tickers.push_back(MakeBookTicker());
  const std::uint64_t expected_local_order_id = LocalOrderIdCodec::Encode(4, 1);
  state.order_responses.push_back(OrderResponseEvent{
      .kind = OrderResponseKind::kAccepted,
      .local_order_id = expected_local_order_id,
      .exchange_order_id = 36028827892199865U,
  });
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = true;
  config.feedback.shm_name = shm_config.shm_name;
  config.feedback.channel_name = shm_config.channel_name;
  config.feedback.poll_budget = 4;
  config.feedback.force_claim = true;

  auto runtime_result = HookLoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeHookOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  EXPECT_EQ(state.bind_runtime_calls, 1);
  EXPECT_EQ(state.set_runtime_hook_calls, 1);
  EXPECT_EQ(state.start_calls, 1);
  EXPECT_EQ(state.stop_calls, 1);
  EXPECT_EQ(state.runtime_hook_calls, 1);
  EXPECT_EQ(state.order_response_poll_calls, 1);
  EXPECT_EQ(state.response_calls, 1);
  EXPECT_EQ(state.feedback_calls, 1);
  EXPECT_EQ(state.data_poll_calls, 0);
  EXPECT_EQ(state.book_ticker_calls, 0);
  EXPECT_EQ(state.placed_local_order_id, expected_local_order_id);
  EXPECT_EQ(state.observed_response_status, OrderStatus::kAccepted);
  EXPECT_EQ(state.last_response_kind, OrderResponseKind::kAccepted);
}

TEST(TradingRuntimeTest, HookModeStartFailureDoesNotCallUserLifecycleHooks) {
  RuntimeLoopState state;
  state.order_start_result = false;
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;

  auto runtime_result = HookLoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeHookOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 1);

  EXPECT_EQ(state.set_runtime_hook_calls, 1);
  EXPECT_EQ(state.start_calls, 1);
  EXPECT_EQ(state.runtime_hook_calls, 0);
  EXPECT_EQ(state.on_start_calls, 0);
  EXPECT_EQ(state.on_stop_calls, 0);
  EXPECT_EQ(state.stop_calls, 1);
}

TEST(TradingRuntimeTest, ProductionRunPollsOrderFeedbackReader) {
  OrderFeedbackShmConfig shm_config{
      .shm_name = "srt_fb_test",
      .channel_name = "srt_fb_ch",
      .create = true,
      .remove_existing = true,
  };
  auto manager_result = OrderFeedbackShmManager::Create(shm_config);
  ASSERT_TRUE(manager_result.ok) << manager_result.error;
  OrderFeedbackShmPublisher publisher(manager_result.value.channel());
  ASSERT_TRUE(publisher.PublishGlobalContinuityLost(
      OrderFeedbackContinuityReason::kSessionDisconnected, 123456));

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

TEST(TradingRuntimeTest, DiagnosticsRecordOrderFeedbackPollEvents) {
  OrderFeedbackShmConfig shm_config{
      .shm_name = "srt_diag_fb_test",
      .channel_name = "srt_diag_fb_ch",
      .create = true,
      .remove_existing = true,
  };
  auto manager_result = OrderFeedbackShmManager::Create(shm_config);
  ASSERT_TRUE(manager_result.ok) << manager_result.error;
  OrderFeedbackShmPublisher publisher(manager_result.value.channel());
  ASSERT_TRUE(publisher.PublishGlobalContinuityLost(
      OrderFeedbackContinuityReason::kSessionDisconnected, 123456));

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

  auto runtime_result = DiagnosticLoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 0);

  const TradingRuntimeLoopStats& stats =
      runtime_result.value->diagnostics().stats();
  EXPECT_EQ(stats.loop_iterations, 1U);
  EXPECT_EQ(stats.order_response_poll_calls, 1U);
  EXPECT_EQ(stats.order_response_empty_polls, 1U);
  EXPECT_EQ(stats.order_feedback_poll_calls, 1U);
  EXPECT_EQ(stats.order_feedback_empty_polls, 0U);
  EXPECT_EQ(stats.order_feedback_events, 1U);
  EXPECT_EQ(stats.data_reader_poll_calls, 0U);
  EXPECT_EQ(stats.data_reader_drain_calls, 0U);
  EXPECT_EQ(stats.data_reader_events, 0U);
}

TEST(TradingRuntimeTest, ProductionRunStopsWhenStrategyShouldStop) {
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

TEST(TradingRuntimeTest,
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

TEST(TradingRuntimeTest, ProductionRunFailsWhenOrderSessionStopsRunning) {
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

TEST(TradingRuntimeTest, ProductionRunPollsOrderResponsesBeforeStoppedExit) {
  RuntimeLoopState state;
  state.order_running = false;
  state.order_responses.push_back(OrderResponseEvent{
      .kind = OrderResponseKind::kUnknownResult,
      .local_order_id = 77,
      .local_receive_ns = 7007,
  });
  g_fake_data_reader_state = &state;
  config::StrategyConfig config = MakeRuntimeConfig();
  config.feedback.enabled = false;

  auto runtime_result = LoopRuntime::Create(
      std::move(config), MakeDataReaderConfig(),
      [&state] { return FakeOrderSession(&state); }, &state);
  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  EXPECT_EQ(runtime_result.value->Run(), 1);

  EXPECT_EQ(state.order_response_poll_calls, 1);
  EXPECT_EQ(state.response_calls, 1);
  EXPECT_EQ(state.next_order_response_index, 1U);
  EXPECT_EQ(state.data_poll_calls, 0);
  EXPECT_EQ(state.book_ticker_calls, 0);
}

}  // namespace
}  // namespace aquila::core
