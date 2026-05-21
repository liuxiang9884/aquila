#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/config/strategy_config.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_types.h"
#include "core/trading/trading_runtime.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/signal.h"
#include "strategy/lead_lag/strategy.h"
#include "strategy/lead_lag/types.h"

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::int64_t> g_counted_allocations{0};
std::atomic<std::int64_t> g_counted_live_bytes{0};

// Counts only allocations made while enabled, so the test can verify
// price_text storage retirement without exposing Strategy internals.
struct AllocationHeader {
  void* raw{nullptr};
  std::size_t size{0};
  bool counted{false};
};

void* AllocateForTest(std::size_t size, std::size_t alignment) {
  if (size == 0) {
    size = 1;
  }
  if (alignment < alignof(void*)) {
    alignment = alignof(void*);
  }
  const std::size_t total = size + sizeof(AllocationHeader) + alignment - 1;
  void* raw = std::malloc(total);
  if (raw == nullptr) {
    throw std::bad_alloc();
  }
  const auto start =
      reinterpret_cast<std::uintptr_t>(raw) + sizeof(AllocationHeader);
  const auto aligned = (start + alignment - 1) & ~(alignment - 1);
  auto* header = reinterpret_cast<AllocationHeader*>(aligned) - 1;
  header->raw = raw;
  header->size = size;
  header->counted = g_count_allocations.load(std::memory_order_relaxed);
  if (header->counted) {
    g_counted_allocations.fetch_add(1, std::memory_order_relaxed);
    g_counted_live_bytes.fetch_add(static_cast<std::int64_t>(size),
                                   std::memory_order_relaxed);
  }
  return reinterpret_cast<void*>(aligned);
}

void DeallocateForTest(void* ptr) noexcept {
  if (ptr == nullptr) {
    return;
  }
  auto* header = reinterpret_cast<AllocationHeader*>(ptr) - 1;
  if (header->counted) {
    g_counted_live_bytes.fetch_sub(static_cast<std::int64_t>(header->size),
                                   std::memory_order_relaxed);
  }
  std::free(header->raw);
}

void StartAllocationCounting() noexcept {
  g_counted_allocations.store(0, std::memory_order_relaxed);
  g_counted_live_bytes.store(0, std::memory_order_relaxed);
  g_count_allocations.store(true, std::memory_order_relaxed);
}

std::int64_t CountedAllocations() noexcept {
  return g_counted_allocations.load(std::memory_order_relaxed);
}

std::int64_t StopAllocationCounting() noexcept {
  g_count_allocations.store(false, std::memory_order_relaxed);
  return g_counted_live_bytes.load(std::memory_order_relaxed);
}

class AllocationCountingGuard {
 public:
  AllocationCountingGuard() noexcept {
    StartAllocationCounting();
  }

  AllocationCountingGuard(const AllocationCountingGuard&) = delete;
  AllocationCountingGuard& operator=(const AllocationCountingGuard&) = delete;

  ~AllocationCountingGuard() {
    Stop();
  }

  [[nodiscard]] std::int64_t allocations() const noexcept {
    return CountedAllocations();
  }

  std::int64_t Stop() noexcept {
    if (!stopped_) {
      live_bytes_ = StopAllocationCounting();
      stopped_ = true;
    }
    return live_bytes_;
  }

 private:
  bool stopped_{false};
  std::int64_t live_bytes_{0};
};

}  // namespace

void* operator new(std::size_t size) {
  return AllocateForTest(size, alignof(std::max_align_t));
}

void* operator new[](std::size_t size) {
  return AllocateForTest(size, alignof(std::max_align_t));
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateForTest(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateForTest(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return AllocateForTest(size, alignof(std::max_align_t));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return AllocateForTest(size, alignof(std::max_align_t));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* ptr) noexcept {
  DeallocateForTest(ptr);
}

void operator delete[](void* ptr) noexcept {
  DeallocateForTest(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
  DeallocateForTest(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
  DeallocateForTest(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
  DeallocateForTest(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
  DeallocateForTest(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
  DeallocateForTest(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
  DeallocateForTest(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  DeallocateForTest(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  DeallocateForTest(ptr);
}

namespace {

namespace leadlag = aquila::strategy::leadlag;

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kOk};
  };

  struct CapturedOrder {
    std::uint64_t local_order_id{0};
    aquila::Exchange exchange{aquila::Exchange::kGate};
    std::int32_t symbol_id{0};
    std::string symbol;
    aquila::OrderSide side{aquila::OrderSide::kBuy};
    aquila::OrderType order_type{aquila::OrderType::kLimit};
    aquila::TimeInForce time_in_force{aquila::TimeInForce::kGoodTillCancel};
    std::int64_t quantity{0};
    std::string price_text;
    bool reduce_only{false};
  };

  SendResult PlaceOrder(aquila::core::StrategyOrder& order) noexcept {
    placed_orders.push_back(CapturedOrder{
        .local_order_id = order.local_order_id,
        .exchange = order.exchange,
        .symbol_id = order.symbol_id,
        .symbol = std::string(order.symbol),
        .side = order.side,
        .order_type = order.type,
        .time_in_force = order.time_in_force,
        .quantity = order.quantity,
        .price_text =
            capture_price_text ? std::string(order.price_text) : std::string{},
        .reduce_only = order.reduce_only,
    });
    return {.status = next_place_status};
  }

  SendResult CancelOrder(aquila::core::StrategyOrder&) noexcept {
    return {};
  }

  SendStatus next_place_status{SendStatus::kOk};
  bool capture_price_text{true};
  std::vector<CapturedOrder> placed_orders;
};

using OrderManagerT = aquila::core::OrderManager<FakeOrderSession>;
using ContextT = aquila::core::StrategyContext<FakeOrderSession>;

leadlag::Config OnePairConfig() {
  leadlag::Config config;
  config.name = "lead_lag";
  config.version = "1.0";
  config.pairs.push_back(leadlag::PairConfig{
      .symbol = "BTC_USDT",
      .symbol_id = 3,
      .lead_exchange = aquila::Exchange::kBinance,
      .lag_exchange = aquila::Exchange::kGate,
  });
  return config;
}

leadlag::Config SignalOnlyConfig() {
  leadlag::Config config;
  config.name = "lead_lag";
  config.version = "1.0";
  config.pairs.push_back(leadlag::PairConfig{
      .symbol = "BTC_USDT",
      .symbol_id = 3,
      .lead_exchange = aquila::Exchange::kBinance,
      .lag_exchange = aquila::Exchange::kGate,
      .lag_taker_fee = 0.0,
      .trigger =
          leadlag::TriggerConfig{
              .lead = 0.02,
              .close = 0.005,
              .lag_part = 0.5,
              .target_profit_rate = 0.0,
              .drift_limit = 1.0,
              .drift_period_ns = 1'000'000'000,
              .drift_min_samples = 1,
              .drift_warmup_ns = 1,
              .quantile =
                  leadlag::QuantileConfig{
                      .move = 0.75,
                      .up_min = 0.0,
                      .up_max = 0.10,
                      .down_min = -0.10,
                      .down_max = 0.0,
                      .precision = 0.000001,
                  },
          },
      .execute =
          leadlag::ExecuteConfig{
              .open_notional = 1000.0,
              .trailing_stop = 0.05,
              .max_entry_spread = 0.02,
              .parallel = 1,
          },
      .bbo_record =
          leadlag::BboRecordConfig{
              .window_ns = 1'000'000'000,
              .stats_window_ns = 1'000'000'000,
          },
      .lag_instrument =
          leadlag::InstrumentMetadata{
              .exchange = aquila::Exchange::kGate,
              .exchange_symbol = "BTC_USDT_GATE",
              .price_tick = 0.1,
              .price_decimal_places = 1,
              .quantity_step = 1.0,
              .min_quantity = 1.0,
              .max_quantity = 20.0,
              .notional_multiplier = 1.0,
          },
  });
  return config;
}

aquila::config::StrategyConfig RuntimeConfig() {
  aquila::config::StrategyConfig config;
  config.name = "lead_lag";
  config.strategy_id = 4;
  config.order_capacity = 8;
  config.feedback.enabled = false;
  return config;
}

aquila::BookTicker Ticker(std::int32_t symbol_id, aquila::Exchange exchange,
                          std::int64_t local_ns, double bid_price,
                          double ask_price) {
  return aquila::BookTicker{
      .id = local_ns,
      .symbol_id = symbol_id,
      .exchange = exchange,
      .exchange_ns = local_ns - 10,
      .local_ns = local_ns,
      .bid_price = bid_price,
      .bid_volume = 1.0,
      .ask_price = ask_price,
      .ask_volume = 1.0,
  };
}

void FeedOpenLongSignal(leadlag::Strategy* strategy, ContextT* context) {
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kGate, 100, 101.57, 102.02), *context);
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), *context);
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), *context);
}

void FeedHugeOpenLongSignal(leadlag::Strategy* strategy, ContextT* context) {
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kGate, 100, 1.0157e64, 1.0202e64),
      *context);
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 1.0e64, 1.01e64),
      *context);
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 1.12e64, 1.13e64),
      *context);
}

void FeedOpenShortSignal(leadlag::Strategy* strategy, ContextT* context) {
  strategy->OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 97.99, 99.79),
                         *context);
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), *context);
  strategy->OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 101, 90.0, 91.0),
                         *context);
}

aquila::OrderFeedbackEvent FilledFeedback(std::uint64_t local_order_id,
                                          std::int64_t quantity,
                                          double fill_price) {
  return aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kFilled,
      .local_order_id = local_order_id,
      .exchange_order_id = local_order_id + 1000,
      .cumulative_filled_quantity = quantity,
      .left_quantity = 0,
      .cancelled_quantity = 0,
      .fill_price = fill_price,
      .role = aquila::OrderRole::kTaker,
      .finish_reason = aquila::OrderFinishReason::kImmediateOrCancel,
      .reject_reason = aquila::OrderRejectReason::kUnknown,
      .continuity_scope = aquila::OrderFeedbackContinuityScope::kLane,
      .continuity_reason = aquila::OrderFeedbackContinuityReason::kUnknown,
      .continuity_sequence = 0,
      .exchange_update_ns = 200,
      .local_receive_ns = 201,
  };
}

aquila::OrderFeedbackEvent PartialFilledFeedback(
    std::uint64_t local_order_id, std::int64_t cumulative_quantity,
    std::int64_t left_quantity, double fill_price) {
  return aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kPartialFilled,
      .local_order_id = local_order_id,
      .exchange_order_id = local_order_id + 1000,
      .cumulative_filled_quantity = cumulative_quantity,
      .left_quantity = left_quantity,
      .cancelled_quantity = 0,
      .fill_price = fill_price,
      .role = aquila::OrderRole::kTaker,
      .finish_reason = aquila::OrderFinishReason::kUnknown,
      .reject_reason = aquila::OrderRejectReason::kUnknown,
      .continuity_scope = aquila::OrderFeedbackContinuityScope::kLane,
      .continuity_reason = aquila::OrderFeedbackContinuityReason::kUnknown,
      .continuity_sequence = 0,
      .exchange_update_ns = 200,
      .local_receive_ns = 201,
  };
}

aquila::OrderFeedbackEvent CancelledFeedback(std::uint64_t local_order_id,
                                             std::int64_t cumulative_quantity,
                                             std::int64_t cancelled_quantity,
                                             double fill_price) {
  return aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kCancelled,
      .local_order_id = local_order_id,
      .exchange_order_id = local_order_id + 1000,
      .cumulative_filled_quantity = cumulative_quantity,
      .left_quantity = cancelled_quantity,
      .cancelled_quantity = cancelled_quantity,
      .fill_price = fill_price,
      .role = aquila::OrderRole::kTaker,
      .finish_reason = aquila::OrderFinishReason::kImmediateOrCancel,
      .reject_reason = aquila::OrderRejectReason::kUnknown,
      .continuity_scope = aquila::OrderFeedbackContinuityScope::kLane,
      .continuity_reason = aquila::OrderFeedbackContinuityReason::kUnknown,
      .continuity_sequence = 0,
      .exchange_update_ns = 200,
      .local_receive_ns = 201,
  };
}

void ApplyFeedback(leadlag::Strategy* strategy, OrderManagerT* order_manager,
                   ContextT* context, const aquila::OrderFeedbackEvent& event) {
  order_manager->OnOrderFeedback(event);
  strategy->OnOrderFeedback(event, *context);
}

void ApplyResponse(leadlag::Strategy* strategy, OrderManagerT* order_manager,
                   ContextT* context,
                   const aquila::core::OrderResponseEvent& event) {
  order_manager->OnOrderResponse(event);
  strategy->OnOrderResponse(event, *context);
}

TEST(LeadLagStrategyInterfaceTest, RuntimeCanDispatchHooks) {
  using Runtime =
      aquila::core::TradingRuntime<leadlag::Strategy, FakeOrderSession>;

  auto runtime_result = Runtime::CreateForTest(
      RuntimeConfig(), [] { return FakeOrderSession{}; }, OnePairConfig());

  ASSERT_TRUE(runtime_result.ok) << runtime_result.error;
  ASSERT_NE(runtime_result.value, nullptr);

  runtime_result.value->HandleBookTickerForTest(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0));
  runtime_result.value->HandleBookTickerForTest(
      Ticker(3, aquila::Exchange::kGate, 110, 99.5, 100.5));
  runtime_result.value->HandleOrderResponseForTest(
      aquila::core::OrderResponseEvent{
          .kind = aquila::core::OrderResponseKind::kAck,
          .local_order_id = 0,
      });
  runtime_result.value->HandleOrderFeedbackForTest(aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kContinuityLost,
      .continuity_scope = aquila::OrderFeedbackContinuityScope::kLane,
      .continuity_reason =
          aquila::OrderFeedbackContinuityReason::kSessionDisconnected,
  });
}

TEST(LeadLagStrategyInterfaceTest, StoresRawMarketUpdates) {
  leadlag::Strategy strategy{OnePairConfig()};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 110, 99.5, 100.5),
                        context);

  EXPECT_EQ(strategy.last_market_update().role, leadlag::PairRole::kLag);
  EXPECT_TRUE(strategy.last_market_update().both_sides_valid);
  ASSERT_NE(strategy.raw_market_state().FindPair(3), nullptr);

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kContinuityLost,
      },
      context);
  EXPECT_TRUE(strategy.degraded());
  EXPECT_FALSE(strategy.ShouldStop());
}

TEST(LeadLagStrategyInterfaceTest, LeadTickEmitsOpenSignalAfterAlignment) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kOpenLong);
  EXPECT_NE(decision.group_id, 0U);
  EXPECT_DOUBLE_EQ(decision.trailing_price, 0.0);
  EXPECT_EQ(decision.intent.side, aquila::OrderSide::kBuy);
  EXPECT_FALSE(decision.intent.reduce_only);
  EXPECT_EQ(decision.intent.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(decision.intent.symbol_id, 3);
  EXPECT_TRUE(strategy.last_signal_diagnostics_valid());

  const leadlag::SignalDiagnostics& diagnostics =
      strategy.last_signal_diagnostics();
  EXPECT_EQ(diagnostics.role, leadlag::PairRole::kLead);
  EXPECT_TRUE(diagnostics.price_changed);
  EXPECT_EQ(diagnostics.event_ns, 91);
  EXPECT_EQ(diagnostics.lead_raw.event_ns, 91);
  EXPECT_DOUBLE_EQ(diagnostics.lead_raw.bid_price, 112.0);
  EXPECT_DOUBLE_EQ(diagnostics.lead_raw.ask_price, 113.0);
  EXPECT_EQ(diagnostics.lag.event_ns, 90);
  EXPECT_DOUBLE_EQ(diagnostics.lag.bid_price, 101.5);
  EXPECT_DOUBLE_EQ(diagnostics.lag.ask_price, 102.0);
  EXPECT_EQ(diagnostics.position_direction, leadlag::PositionDirection::kLong);
  EXPECT_EQ(diagnostics.active_group_count, 1U);
  EXPECT_EQ(diagnostics.group_id, decision.group_id);
  EXPECT_DOUBLE_EQ(diagnostics.trailing_price, 0.0);
}

TEST(LeadLagStrategyInterfaceTest, DefaultModeDoesNotCreateSyntheticHold) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);
  ASSERT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  EXPECT_FALSE(decision.triggered);
  EXPECT_NE(decision.action, leadlag::SignalAction::kCloseLong);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModePlacesIocLimitOrderOnOpenSignal) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const FakeOrderSession::CapturedOrder& order =
      order_session.placed_orders.back();
  EXPECT_EQ(order.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(order.symbol_id, 3);
  EXPECT_EQ(order.symbol, "BTC_USDT_GATE");
  EXPECT_EQ(order.side, aquila::OrderSide::kBuy);
  EXPECT_EQ(order.order_type, aquila::OrderType::kLimit);
  EXPECT_EQ(order.time_in_force, aquila::TimeInForce::kImmediateOrCancel);
  EXPECT_FALSE(order.reduce_only);
  EXPECT_EQ(order.price_text, "102.1");
  EXPECT_EQ(order.quantity, 9);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kOpenLong);
  EXPECT_NE(decision.group_id, 0U);
  ASSERT_TRUE(strategy.last_signal_diagnostics_valid());
  EXPECT_EQ(strategy.last_signal_diagnostics().group_id, decision.group_id);
  EXPECT_EQ(strategy.last_signal_diagnostics().active_group_count, 1U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeUsesFilledPositionQuantityForCloseOrder) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));
  EXPECT_EQ(context.FindOrder(open_order_id), nullptr);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const FakeOrderSession::CapturedOrder& close_order =
      order_session.placed_orders.back();
  EXPECT_EQ(close_order.side, aquila::OrderSide::kSell);
  EXPECT_EQ(close_order.order_type, aquila::OrderType::kLimit);
  EXPECT_EQ(close_order.time_in_force, aquila::TimeInForce::kImmediateOrCancel);
  EXPECT_TRUE(close_order.reduce_only);
  EXPECT_EQ(close_order.price_text, "101.5");
  EXPECT_EQ(close_order.quantity, 7);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kCloseLong);
  EXPECT_NE(decision.group_id, 0U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeKeepsPartialOpenFeedbackPending) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;

  ApplyFeedback(&strategy, &order_manager, &context,
                PartialFilledFeedback(open_order_id, 3, 4, 102.1));

  const aquila::core::StrategyOrder* open_order =
      context.FindOrder(open_order_id);
  ASSERT_NE(open_order, nullptr);
  EXPECT_EQ(open_order->status, aquila::core::OrderStatus::kPartialFilled);
  EXPECT_FALSE(open_order->is_finished);
  EXPECT_EQ(open_order->cumulative_filled_quantity, 3);
  EXPECT_EQ(order_manager.order_count(), 1U);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  EXPECT_EQ(order_session.placed_orders.size(), 1U);
}

TEST(LeadLagStrategyInterfaceTest, ExternalModeRetiresTerminalCloseFeedback) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);
  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t close_order_id =
      order_session.placed_orders.back().local_order_id;

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(close_order_id, 7, 101.5));

  EXPECT_EQ(context.FindOrder(close_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 103, 99.0, 100.0),
                        context);
  EXPECT_EQ(order_session.placed_orders.size(), 2U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRetriesCloseAfterTerminalCancelledWithoutFill) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);
  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t cancelled_close_order_id =
      order_session.placed_orders.back().local_order_id;

  ApplyFeedback(&strategy, &order_manager, &context,
                CancelledFeedback(cancelled_close_order_id, 0, 7, 0.0));

  EXPECT_EQ(context.FindOrder(cancelled_close_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 103, 99.0, 100.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 3U);
  const FakeOrderSession::CapturedOrder& retry_close =
      order_session.placed_orders.back();
  EXPECT_NE(retry_close.local_order_id, cancelled_close_order_id);
  EXPECT_EQ(retry_close.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(retry_close.reduce_only);
  EXPECT_EQ(retry_close.quantity, 7);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRetriesCloseRemainingAfterPartialTerminalCancelled) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);
  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t partial_close_order_id =
      order_session.placed_orders.back().local_order_id;

  ApplyFeedback(&strategy, &order_manager, &context,
                CancelledFeedback(partial_close_order_id, 3, 4, 101.5));

  EXPECT_EQ(context.FindOrder(partial_close_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 103, 99.0, 100.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 3U);
  const FakeOrderSession::CapturedOrder& retry_close =
      order_session.placed_orders.back();
  EXPECT_NE(retry_close.local_order_id, partial_close_order_id);
  EXPECT_EQ(retry_close.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(retry_close.reduce_only);
  EXPECT_EQ(retry_close.quantity, 4);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeUsesFilledPositionQuantityForStoplossOrder) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 11, 102.1));

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 95.0, 96.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const FakeOrderSession::CapturedOrder& stoploss_order =
      order_session.placed_orders.back();
  EXPECT_EQ(stoploss_order.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(stoploss_order.reduce_only);
  EXPECT_EQ(stoploss_order.price_text, "94.5");
  EXPECT_EQ(stoploss_order.quantity, 11);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kStoplossLong);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRetiresTerminalStoplossFeedback) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 11, 102.1));

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 95.0, 96.0),
                        context);
  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t stoploss_order_id =
      order_session.placed_orders.back().local_order_id;

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(stoploss_order_id, 11, 94.5));

  EXPECT_EQ(context.FindOrder(stoploss_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 103, 90.0, 91.0),
                        context);
  EXPECT_EQ(order_session.placed_orders.size(), 2U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRetriesStoplossAfterTerminalCancelledWithoutFill) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 11, 102.1));

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 95.0, 96.0),
                        context);
  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t cancelled_stoploss_order_id =
      order_session.placed_orders.back().local_order_id;

  ApplyFeedback(&strategy, &order_manager, &context,
                CancelledFeedback(cancelled_stoploss_order_id, 0, 11, 0.0));

  EXPECT_EQ(context.FindOrder(cancelled_stoploss_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 103, 90.0, 91.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 3U);
  const FakeOrderSession::CapturedOrder& retry_stoploss =
      order_session.placed_orders.back();
  EXPECT_NE(retry_stoploss.local_order_id, cancelled_stoploss_order_id);
  EXPECT_EQ(retry_stoploss.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(retry_stoploss.reduce_only);
  EXPECT_EQ(retry_stoploss.quantity, 11);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRollsBackSessionRejectedOpenOrder) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  order_session.next_place_status = FakeOrderSession::SendStatus::kRejected;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t rejected_order_id =
      order_session.placed_orders.back().local_order_id;
  EXPECT_EQ(context.FindOrder(rejected_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  order_session.next_place_status = FakeOrderSession::SendStatus::kOk;
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 102, 80.0, 81.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  EXPECT_NE(order_session.placed_orders.back().local_order_id,
            rejected_order_id);
  EXPECT_TRUE(strategy.last_signal_decision().triggered);
  EXPECT_NE(strategy.last_signal_decision().group_id, 0U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRollsBackResponseRejectedOpenOrder) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t rejected_order_id =
      order_session.placed_orders.back().local_order_id;

  const aquila::core::OrderResponseEvent rejected{
      .kind = aquila::core::OrderResponseKind::kRejected,
      .local_order_id = rejected_order_id,
  };
  order_manager.OnOrderResponse(rejected);
  strategy.OnOrderResponse(rejected, context);

  EXPECT_EQ(context.FindOrder(rejected_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 102, 80.0, 81.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  EXPECT_NE(order_session.placed_orders.back().local_order_id,
            rejected_order_id);
  EXPECT_TRUE(strategy.last_signal_decision().triggered);
  EXPECT_NE(strategy.last_signal_decision().group_id, 0U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRejectedCloseReturnsHoldAndCanRetry) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);
  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t rejected_close_order_id =
      order_session.placed_orders.back().local_order_id;

  ApplyResponse(&strategy, &order_manager, &context,
                aquila::core::OrderResponseEvent{
                    .kind = aquila::core::OrderResponseKind::kRejected,
                    .local_order_id = rejected_close_order_id,
                });

  EXPECT_EQ(context.FindOrder(rejected_close_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 103, 99.0, 100.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 3U);
  const FakeOrderSession::CapturedOrder& retry_close =
      order_session.placed_orders.back();
  EXPECT_NE(retry_close.local_order_id, rejected_close_order_id);
  EXPECT_EQ(retry_close.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(retry_close.reduce_only);
  EXPECT_EQ(retry_close.quantity, 7);
}

TEST(LeadLagStrategyInterfaceTest,
     RetiredRejectedOrdersDoNotAllocatePriceTextStorageInOrderPath) {
  leadlag::Config config = SignalOnlyConfig();
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  order_session.next_place_status = FakeOrderSession::SendStatus::kRejected;
  order_session.capture_price_text = false;
  order_session.placed_orders.reserve(64);
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.57, 102.02),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);

  AllocationCountingGuard allocations;
  for (int i = 0; i < 64; ++i) {
    const double lead_bid = 112.0 + static_cast<double>(i);
    strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 101 + i,
                                 lead_bid, lead_bid + 1.0),
                          context);
  }
  const std::int64_t counted_allocations = allocations.allocations();
  const std::int64_t live_bytes = allocations.Stop();

  ASSERT_GE(order_session.placed_orders.size(), 32U);
  EXPECT_EQ(order_manager.order_count(), 0U);
  EXPECT_EQ(counted_allocations, 0)
      << "price_text storage allocated in the per-order path";
  EXPECT_EQ(live_bytes, 0) << "retired price_text storage still retained";
}

TEST(LeadLagStrategyInterfaceTest,
     PoolFullOrderPathReleasesPriceTextSlotForNextContext) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession pool_full_session;
  OrderManagerT pool_full_manager{pool_full_session, 0, 4};
  ContextT pool_full_context{pool_full_manager};

  {
    AllocationCountingGuard allocations;
    FeedOpenLongSignal(&strategy, &pool_full_context);
    const std::int64_t counted_allocations = allocations.allocations();
    const std::int64_t live_bytes = allocations.Stop();
    EXPECT_EQ(counted_allocations, 0)
        << "pool-full price_text path allocated per order";
    EXPECT_EQ(live_bytes, 0) << "pool-full price_text storage still retained";
  }

  ASSERT_TRUE(strategy.last_signal_decision().triggered);
  EXPECT_TRUE(pool_full_session.placed_orders.empty());
  EXPECT_EQ(pool_full_manager.order_count(), 0U);

  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 102, 80.0, 81.0),
                        context);

  ASSERT_TRUE(strategy.last_signal_decision().triggered)
      << static_cast<int>(strategy.last_signal_decision().reject_reason);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  EXPECT_NE(order_session.placed_orders.back().local_order_id, 0U);
  EXPECT_EQ(order_manager.order_count(), 1U);
}

TEST(LeadLagStrategyInterfaceTest, ExternalModeClampsOpenQuantityToMax) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].execute.open_notional = 100000.0;
  config.pairs[0].lag_instrument.max_quantity = 5.0;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  EXPECT_EQ(order_session.placed_orders.back().quantity, 5);
}

TEST(LeadLagStrategyInterfaceTest, ExternalModeSkipsOpenBelowMinQuantity) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].execute.open_notional = 10.0;
  config.pairs[0].lag_instrument.min_quantity = 1.0;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  EXPECT_TRUE(strategy.last_signal_decision().triggered);
  EXPECT_TRUE(order_session.placed_orders.empty());
  EXPECT_EQ(order_manager.order_count(), 0U);
}

TEST(LeadLagStrategyInterfaceTest, ExternalModeSkipsOrderWhenPriceTextInvalid) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].lag_instrument.price_decimal_places = 128;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  EXPECT_TRUE(strategy.last_signal_decision().triggered);
  EXPECT_TRUE(order_session.placed_orders.empty());
  EXPECT_EQ(order_manager.order_count(), 0U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeSkipsOrderWhenPriceTextBufferTooSmall) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].execute.open_notional = 1.0e66;
  config.pairs[0].lag_instrument.price_tick = 1.0;
  config.pairs[0].lag_instrument.price_decimal_places = 18;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedHugeOpenLongSignal(&strategy, &context);

  EXPECT_TRUE(strategy.last_signal_decision().triggered)
      << static_cast<int>(strategy.last_signal_decision().reject_reason);
  EXPECT_TRUE(order_session.placed_orders.empty());
  EXPECT_EQ(order_manager.order_count(), 0U);
}

TEST(LeadLagStrategyInterfaceTest, ExternalModeOpenShortUsesSellPriceFloor) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenShortSignal(&strategy, &context);

  EXPECT_TRUE(strategy.last_signal_decision().triggered)
      << static_cast<int>(strategy.last_signal_decision().reject_reason);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const FakeOrderSession::CapturedOrder& order =
      order_session.placed_orders.back();
  EXPECT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenShort);
  EXPECT_EQ(order.side, aquila::OrderSide::kSell);
  EXPECT_FALSE(order.reduce_only);
  EXPECT_EQ(order.price_text, "97.9");
  EXPECT_EQ(order.quantity, 10);
}

TEST(LeadLagStrategyInterfaceTest, ExternalModeStoplossShortUsesBuyPriceCeil) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenShortSignal(&strategy, &context);
  EXPECT_TRUE(strategy.last_signal_decision().triggered)
      << static_cast<int>(strategy.last_signal_decision().reject_reason);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 10, 97.9));

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 102.1, 103.03),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const FakeOrderSession::CapturedOrder& stoploss_order =
      order_session.placed_orders.back();
  EXPECT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kStoplossShort);
  EXPECT_EQ(stoploss_order.side, aquila::OrderSide::kBuy);
  EXPECT_TRUE(stoploss_order.reduce_only);
  EXPECT_EQ(stoploss_order.price_text, "103.6");
  EXPECT_EQ(stoploss_order.quantity, 10);
}

TEST(LeadLagStrategyInterfaceTest, ReplayModeEmitsCloseSignalForSyntheticHold) {
  leadlag::Strategy strategy{
      SignalOnlyConfig(),
      leadlag::StrategyOptions{
          .position_accounting =
              leadlag::PositionAccountingMode::kSyntheticSignals,
      }};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);
  ASSERT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kCloseLong);
  EXPECT_NE(decision.group_id, 0U);
  EXPECT_DOUBLE_EQ(decision.trailing_price, 102.0);
  EXPECT_EQ(decision.intent.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(decision.intent.reduce_only);

  EXPECT_TRUE(strategy.last_signal_diagnostics_valid());
  const leadlag::SignalDiagnostics& diagnostics =
      strategy.last_signal_diagnostics();
  EXPECT_EQ(diagnostics.group_id, decision.group_id);
  EXPECT_DOUBLE_EQ(diagnostics.trailing_price, 102.0);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  EXPECT_FALSE(strategy.last_signal_diagnostics_valid());
}

TEST(LeadLagStrategyInterfaceTest,
     ReplayModeClearsTriggeredSyntheticGroupById) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].execute.parallel = 2;
  leadlag::Strategy strategy{
      config, leadlag::StrategyOptions{
                  .position_accounting =
                      leadlag::PositionAccountingMode::kSyntheticSignals,
              }};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);
  ASSERT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 105.0, 106.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 103, 170.0, 171.0), context);
  ASSERT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 104, 100.4, 101.4),
                        context);

  const leadlag::SignalDecision& stoploss = strategy.last_signal_decision();
  ASSERT_TRUE(stoploss.triggered);
  ASSERT_EQ(stoploss.action, leadlag::SignalAction::kStoplossLong);
  ASSERT_NE(stoploss.group_id, 0U);
  const std::uint64_t stopped_group_id = stoploss.group_id;
  EXPECT_DOUBLE_EQ(stoploss.trailing_price, 106.0);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 106, 100.0, 101.0), context);

  const leadlag::SignalDecision& close = strategy.last_signal_decision();
  ASSERT_TRUE(close.triggered) << static_cast<int>(close.reject_reason);
  ASSERT_EQ(close.action, leadlag::SignalAction::kCloseLong);
  EXPECT_NE(close.group_id, stopped_group_id);
}

TEST(LeadLagStrategyInterfaceTest, FeedbackContinuityLostPausesNewOpenSignals) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  aquila::core::OrderManager<FakeOrderSession> order_manager{order_session, 8,
                                                             4};
  aquila::core::StrategyContext<FakeOrderSession> context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kContinuityLost,
      },
      context);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  EXPECT_FALSE(decision.triggered);
  EXPECT_EQ(decision.reject_reason, leadlag::SignalRejectReason::kDegraded);
  EXPECT_TRUE(strategy.degraded());
}

}  // namespace
