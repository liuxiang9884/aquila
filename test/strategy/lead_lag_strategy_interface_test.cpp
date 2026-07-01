#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
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
#include "nova/utils/log.h"
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

constexpr std::int32_t kWideFreshnessGuardMs = 2'000'000'000;
constexpr std::uint64_t kWideFreshnessGuardNs =
    static_cast<std::uint64_t>(kWideFreshnessGuardMs) * 1'000'000ULL;

void EnsureLoggingStarted() {
  static const bool started = [] {
    nova::LogConfig config;
    config.set_console_sink_name("");
    config.set_file_sink_name((std::filesystem::temp_directory_path() /
                               "aquila_lead_lag_strategy_interface_test.log")
                                  .string());
    nova::InitializeLogging(config);
    return true;
  }();
  (void)started;
}

void FlushStrategyLogging() {
  if (nova::kLogManager.logger() != nullptr) {
    nova::kLogManager.logger()->flush_log();
  }
}

class StrategyLoggingEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    EnsureLoggingStarted();
  }
};

[[maybe_unused]] const ::testing::Environment* g_strategy_logging_environment =
    ::testing::AddGlobalTestEnvironment(new StrategyLoggingEnvironment);

std::array<leadlag::detail::StrategyOrderIntentLogRecordForTest, 4>
    g_order_intent_logs{};
std::size_t g_order_intent_log_count{0};
std::array<leadlag::detail::StrategyOrderIntentRejectedLogRecordForTest, 4>
    g_order_intent_rejected_logs{};
std::size_t g_order_intent_rejected_log_count{0};
std::array<leadlag::detail::StrategyOrderSubmittedLogRecordForTest, 4>
    g_order_submitted_logs{};
std::size_t g_order_submitted_log_count{0};
std::array<leadlag::detail::StrategyOrderResponseLogRecordForTest, 4>
    g_order_response_logs{};
std::size_t g_order_response_log_count{0};
std::array<leadlag::detail::StrategyOrderFeedbackLogRecordForTest, 4>
    g_order_feedback_logs{};
std::size_t g_order_feedback_log_count{0};
std::array<leadlag::detail::StrategyOrderFinishedLogRecordForTest, 4>
    g_order_finished_logs{};
std::size_t g_order_finished_log_count{0};
std::array<leadlag::detail::StrategySignalTriggeredLogRecordForTest, 4>
    g_signal_triggered_logs{};
std::size_t g_signal_triggered_log_count{0};
std::array<leadlag::detail::StrategySignalDecisionLogRecordForTest, 4>
    g_signal_decision_logs{};
std::size_t g_signal_decision_log_count{0};

void CaptureStrategyOrderIntentLogForTest(
    const leadlag::detail::StrategyOrderIntentLogRecordForTest&
        record) noexcept {
  if (g_order_intent_log_count >= g_order_intent_logs.size()) {
    return;
  }
  g_order_intent_logs[g_order_intent_log_count] = record;
  ++g_order_intent_log_count;
}

void ResetStrategyOrderIntentLogCapture() noexcept {
  g_order_intent_logs = {};
  g_order_intent_log_count = 0;
  leadlag::detail::SetStrategyOrderIntentLogObserverForTest(nullptr);
}

void CaptureStrategyOrderIntentRejectedLogForTest(
    const leadlag::detail::StrategyOrderIntentRejectedLogRecordForTest&
        record) noexcept {
  if (g_order_intent_rejected_log_count >=
      g_order_intent_rejected_logs.size()) {
    return;
  }
  g_order_intent_rejected_logs[g_order_intent_rejected_log_count] = record;
  ++g_order_intent_rejected_log_count;
}

void ResetStrategyOrderIntentRejectedLogCapture() noexcept {
  g_order_intent_rejected_logs = {};
  g_order_intent_rejected_log_count = 0;
  leadlag::detail::SetStrategyOrderIntentRejectedLogObserverForTest(nullptr);
}

void CaptureStrategyOrderSubmittedLogForTest(
    const leadlag::detail::StrategyOrderSubmittedLogRecordForTest&
        record) noexcept {
  if (g_order_submitted_log_count >= g_order_submitted_logs.size()) {
    return;
  }
  g_order_submitted_logs[g_order_submitted_log_count] = record;
  ++g_order_submitted_log_count;
}

void ResetStrategyOrderSubmittedLogCapture() noexcept {
  g_order_submitted_logs = {};
  g_order_submitted_log_count = 0;
  leadlag::detail::SetStrategyOrderSubmittedLogObserverForTest(nullptr);
}

void CaptureStrategyOrderResponseLogForTest(
    const leadlag::detail::StrategyOrderResponseLogRecordForTest&
        record) noexcept {
  if (g_order_response_log_count >= g_order_response_logs.size()) {
    return;
  }
  g_order_response_logs[g_order_response_log_count] = record;
  ++g_order_response_log_count;
}

void ResetStrategyOrderResponseLogCapture() noexcept {
  g_order_response_logs = {};
  g_order_response_log_count = 0;
  leadlag::detail::SetStrategyOrderResponseLogObserverForTest(nullptr);
}

void CaptureStrategyOrderFeedbackLogForTest(
    const leadlag::detail::StrategyOrderFeedbackLogRecordForTest&
        record) noexcept {
  if (g_order_feedback_log_count >= g_order_feedback_logs.size()) {
    return;
  }
  g_order_feedback_logs[g_order_feedback_log_count] = record;
  ++g_order_feedback_log_count;
}

void ResetStrategyOrderFeedbackLogCapture() noexcept {
  g_order_feedback_logs = {};
  g_order_feedback_log_count = 0;
  leadlag::detail::SetStrategyOrderFeedbackLogObserverForTest(nullptr);
}

void CaptureStrategyOrderFinishedLogForTest(
    const leadlag::detail::StrategyOrderFinishedLogRecordForTest&
        record) noexcept {
  if (g_order_finished_log_count >= g_order_finished_logs.size()) {
    return;
  }
  g_order_finished_logs[g_order_finished_log_count] = record;
  ++g_order_finished_log_count;
}

void ResetStrategyOrderFinishedLogCapture() noexcept {
  g_order_finished_logs = {};
  g_order_finished_log_count = 0;
  leadlag::detail::SetStrategyOrderFinishedLogObserverForTest(nullptr);
}

void CaptureStrategySignalTriggeredLogForTest(
    const leadlag::detail::StrategySignalTriggeredLogRecordForTest&
        record) noexcept {
  if (g_signal_triggered_log_count >= g_signal_triggered_logs.size()) {
    return;
  }
  g_signal_triggered_logs[g_signal_triggered_log_count] = record;
  ++g_signal_triggered_log_count;
}

void ResetStrategySignalTriggeredLogCapture() noexcept {
  g_signal_triggered_logs = {};
  g_signal_triggered_log_count = 0;
  leadlag::detail::SetStrategySignalTriggeredLogObserverForTest(nullptr);
}

void CaptureStrategySignalDecisionLogForTest(
    const leadlag::detail::StrategySignalDecisionLogRecordForTest&
        record) noexcept {
  if (g_signal_decision_log_count >= g_signal_decision_logs.size()) {
    return;
  }
  g_signal_decision_logs[g_signal_decision_log_count] = record;
  ++g_signal_decision_log_count;
}

void ResetStrategySignalDecisionLogCapture() noexcept {
  g_signal_decision_logs = {};
  g_signal_decision_log_count = 0;
  leadlag::detail::SetStrategySignalDecisionLogObserverForTest(nullptr);
}

class StrategyOrderIntentLogCaptureGuard {
 public:
  StrategyOrderIntentLogCaptureGuard() noexcept {
    ResetStrategyOrderIntentLogCapture();
    leadlag::detail::SetStrategyOrderIntentLogObserverForTest(
        CaptureStrategyOrderIntentLogForTest);
  }

  ~StrategyOrderIntentLogCaptureGuard() noexcept {
    ResetStrategyOrderIntentLogCapture();
  }

  StrategyOrderIntentLogCaptureGuard(
      const StrategyOrderIntentLogCaptureGuard&) = delete;
  StrategyOrderIntentLogCaptureGuard& operator=(
      const StrategyOrderIntentLogCaptureGuard&) = delete;
};

class StrategyOrderIntentRejectedLogCaptureGuard {
 public:
  StrategyOrderIntentRejectedLogCaptureGuard() noexcept {
    ResetStrategyOrderIntentRejectedLogCapture();
    leadlag::detail::SetStrategyOrderIntentRejectedLogObserverForTest(
        CaptureStrategyOrderIntentRejectedLogForTest);
  }

  ~StrategyOrderIntentRejectedLogCaptureGuard() noexcept {
    ResetStrategyOrderIntentRejectedLogCapture();
  }

  StrategyOrderIntentRejectedLogCaptureGuard(
      const StrategyOrderIntentRejectedLogCaptureGuard&) = delete;
  StrategyOrderIntentRejectedLogCaptureGuard& operator=(
      const StrategyOrderIntentRejectedLogCaptureGuard&) = delete;
};

class StrategyOrderSubmittedLogCaptureGuard {
 public:
  StrategyOrderSubmittedLogCaptureGuard() noexcept {
    ResetStrategyOrderSubmittedLogCapture();
    leadlag::detail::SetStrategyOrderSubmittedLogObserverForTest(
        CaptureStrategyOrderSubmittedLogForTest);
  }

  ~StrategyOrderSubmittedLogCaptureGuard() noexcept {
    ResetStrategyOrderSubmittedLogCapture();
  }

  StrategyOrderSubmittedLogCaptureGuard(
      const StrategyOrderSubmittedLogCaptureGuard&) = delete;
  StrategyOrderSubmittedLogCaptureGuard& operator=(
      const StrategyOrderSubmittedLogCaptureGuard&) = delete;
};

class StrategyOrderFinishedLogCaptureGuard {
 public:
  StrategyOrderFinishedLogCaptureGuard() noexcept {
    ResetStrategyOrderFinishedLogCapture();
    leadlag::detail::SetStrategyOrderFinishedLogObserverForTest(
        CaptureStrategyOrderFinishedLogForTest);
  }

  ~StrategyOrderFinishedLogCaptureGuard() noexcept {
    ResetStrategyOrderFinishedLogCapture();
  }

  StrategyOrderFinishedLogCaptureGuard(
      const StrategyOrderFinishedLogCaptureGuard&) = delete;
  StrategyOrderFinishedLogCaptureGuard& operator=(
      const StrategyOrderFinishedLogCaptureGuard&) = delete;
};

class StrategyOrderResponseLogCaptureGuard {
 public:
  StrategyOrderResponseLogCaptureGuard() noexcept {
    ResetStrategyOrderResponseLogCapture();
    leadlag::detail::SetStrategyOrderResponseLogObserverForTest(
        CaptureStrategyOrderResponseLogForTest);
  }

  ~StrategyOrderResponseLogCaptureGuard() noexcept {
    ResetStrategyOrderResponseLogCapture();
  }

  StrategyOrderResponseLogCaptureGuard(
      const StrategyOrderResponseLogCaptureGuard&) = delete;
  StrategyOrderResponseLogCaptureGuard& operator=(
      const StrategyOrderResponseLogCaptureGuard&) = delete;
};

class StrategyOrderFeedbackLogCaptureGuard {
 public:
  StrategyOrderFeedbackLogCaptureGuard() noexcept {
    ResetStrategyOrderFeedbackLogCapture();
    leadlag::detail::SetStrategyOrderFeedbackLogObserverForTest(
        CaptureStrategyOrderFeedbackLogForTest);
  }

  ~StrategyOrderFeedbackLogCaptureGuard() noexcept {
    ResetStrategyOrderFeedbackLogCapture();
  }

  StrategyOrderFeedbackLogCaptureGuard(
      const StrategyOrderFeedbackLogCaptureGuard&) = delete;
  StrategyOrderFeedbackLogCaptureGuard& operator=(
      const StrategyOrderFeedbackLogCaptureGuard&) = delete;
};

class StrategySignalTriggeredLogCaptureGuard {
 public:
  StrategySignalTriggeredLogCaptureGuard() noexcept {
    ResetStrategySignalTriggeredLogCapture();
    leadlag::detail::SetStrategySignalTriggeredLogObserverForTest(
        CaptureStrategySignalTriggeredLogForTest);
  }

  ~StrategySignalTriggeredLogCaptureGuard() noexcept {
    ResetStrategySignalTriggeredLogCapture();
  }

  StrategySignalTriggeredLogCaptureGuard(
      const StrategySignalTriggeredLogCaptureGuard&) = delete;
  StrategySignalTriggeredLogCaptureGuard& operator=(
      const StrategySignalTriggeredLogCaptureGuard&) = delete;
};

class StrategySignalDecisionLogCaptureGuard {
 public:
  StrategySignalDecisionLogCaptureGuard() noexcept {
    ResetStrategySignalDecisionLogCapture();
    leadlag::detail::SetStrategySignalDecisionLogObserverForTest(
        CaptureStrategySignalDecisionLogForTest);
  }

  ~StrategySignalDecisionLogCaptureGuard() noexcept {
    ResetStrategySignalDecisionLogCapture();
  }

  StrategySignalDecisionLogCaptureGuard(
      const StrategySignalDecisionLogCaptureGuard&) = delete;
  StrategySignalDecisionLogCaptureGuard& operator=(
      const StrategySignalDecisionLogCaptureGuard&) = delete;
};

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kOk};
  };

  struct CapturedOrder {
    std::uint64_t local_order_id{0};
    std::uint64_t parent_id{0};
    aquila::Exchange exchange{aquila::Exchange::kGate};
    std::int32_t symbol_id{0};
    std::string symbol;
    aquila::OrderSide side{aquila::OrderSide::kBuy};
    aquila::OrderType order_type{aquila::OrderType::kLimit};
    aquila::TimeInForce time_in_force{aquila::TimeInForce::kGoodTillCancel};
    double quantity{0.0};
    std::string quantity_text;
    std::string price_text;
    bool reduce_only{false};
    std::uint16_t gateway_route_id{aquila::core::kAutoGatewayRoute};
  };

  SendResult PlaceOrder(aquila::core::StrategyOrder& order) noexcept {
    placed_orders.push_back(CapturedOrder{
        .local_order_id = order.local_order_id,
        .parent_id = order.parent_id,
        .exchange = order.exchange,
        .symbol_id = order.symbol_id,
        .symbol = std::string(order.symbol),
        .side = order.side,
        .order_type = order.type,
        .time_in_force = order.time_in_force,
        .quantity = order.quantity,
        .quantity_text = std::string(order.quantity_text),
        .price_text =
            capture_price_text ? std::string(order.price_text) : std::string{},
        .reduce_only = order.reduce_only,
        .gateway_route_id = order.gateway_route_id,
    });
    if (reject_next_place_count > 0) {
      --reject_next_place_count;
      return {.status = SendStatus::kRejected};
    }
    return {.status = next_place_status};
  }

  SendResult CancelOrder(aquila::core::StrategyOrder&) noexcept {
    return {};
  }

  [[nodiscard]] std::uint16_t MaxOrderSessionFanout() const noexcept {
    return max_order_session_fanout;
  }

  [[nodiscard]] bool RouteReady(std::uint16_t route_id) const noexcept {
    return route_id < route_ready.size() && route_ready[route_id];
  }

  void RefreshRouteStates() noexcept {
    ++refresh_route_calls;
    if (refresh_routes_marks_route0_not_ready) {
      route_ready[0] = false;
    }
  }

  SendStatus next_place_status{SendStatus::kOk};
  std::uint32_t reject_next_place_count{0};
  bool capture_price_text{true};
  bool refresh_routes_marks_route0_not_ready{false};
  int refresh_route_calls{0};
  std::uint16_t max_order_session_fanout{leadlag::kMaxOrderSessionFanout};
  std::array<bool, leadlag::kMaxOrderSessionFanout> route_ready{
      true, true, true, true, true, true, true, true,
      true, true, true, true, true, true, true, true};
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
      .max_lead_freshness_ms = 5,
      .max_lag_freshness_ms = 20,
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
      .max_lead_freshness_ms = kWideFreshnessGuardMs,
      .max_lag_freshness_ms = kWideFreshnessGuardMs,
      .trigger =
          leadlag::TriggerConfig{
              .lead = 0.02,
              .close = 0.005,
              .lag_part = 0.5,
              .target_profit_rate = 0.0,
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
              .open_slippage_ticks = 0,
              .close_slippage_ticks = 0,
              .stoploss_slippage_ticks = 0,
              .close_retry_times = 0,
              .close_retry_slippage_step_ticks = 0,
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

leadlag::Config SignalOnlyConfigWithDriftGuard(double drift_instant) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].trigger.drift_guard = leadlag::DriftGuardConfig{
      .enabled = true,
      .drift_instant = drift_instant,
      .ratio_std = 10.0,
      .ratio_std_window_ns = 60'000'000'000ULL,
      .drift_mean = 10.0,
      .drift_mean_window_ns = 60'000'000'000ULL,
  };
  return config;
}

struct TriggeredSignalCapture {
  std::uint32_t count{0};
  aquila::BookTicker trigger_ticker;
  leadlag::SignalDecision decision;
  leadlag::SignalDiagnostics diagnostics;
};

void CaptureTriggeredSignal(
    void* context, const aquila::BookTicker& trigger_ticker,
    const leadlag::SignalDecision& decision,
    const leadlag::SignalDiagnostics& diagnostics) noexcept {
  auto* capture = static_cast<TriggeredSignalCapture*>(context);
  ++capture->count;
  capture->trigger_ticker = trigger_ticker;
  capture->decision = decision;
  capture->diagnostics = diagnostics;
}

leadlag::Config SignalOnlyConfigWithSlippage(
    std::uint32_t open_slippage_ticks, std::uint32_t close_slippage_ticks,
    std::uint32_t stoploss_slippage_ticks) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].execute.open_slippage_ticks = open_slippage_ticks;
  config.pairs[0].execute.close_slippage_ticks = close_slippage_ticks;
  config.pairs[0].execute.stoploss_slippage_ticks = stoploss_slippage_ticks;
  return config;
}

leadlag::Config SignalOnlyConfigWithSlippage(
    std::uint32_t open_slippage_ticks, std::uint32_t close_slippage_ticks) {
  return SignalOnlyConfigWithSlippage(open_slippage_ticks, close_slippage_ticks,
                                      close_slippage_ticks);
}

leadlag::Config SignalOnlyConfigWithCloseRetry(
    std::uint32_t close_slippage_ticks, std::uint32_t stoploss_slippage_ticks,
    std::uint32_t close_retry_times,
    std::uint32_t close_retry_slippage_step_ticks) {
  leadlag::Config config = SignalOnlyConfigWithSlippage(
      /*open_slippage_ticks=*/0, close_slippage_ticks, stoploss_slippage_ticks);
  config.pairs[0].execute.close_retry_times = close_retry_times;
  config.pairs[0].execute.close_retry_slippage_step_ticks =
      close_retry_slippage_step_ticks;
  return config;
}

leadlag::Config SignalOnlyConfigWithFanout(std::uint32_t fanout) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].execute.order_session_fanout = fanout;
  return config;
}

leadlag::Config SignalOnlyConfigWithCloseRetryAndFanout(
    std::uint32_t close_slippage_ticks, std::uint32_t stoploss_slippage_ticks,
    std::uint32_t close_retry_times,
    std::uint32_t close_retry_slippage_step_ticks, std::uint32_t fanout) {
  leadlag::Config config = SignalOnlyConfigWithCloseRetry(
      close_slippage_ticks, stoploss_slippage_ticks, close_retry_times,
      close_retry_slippage_step_ticks);
  config.pairs[0].execute.order_session_fanout = fanout;
  return config;
}

leadlag::Config SignalOnlyConfigWithTakerBufferShadow() {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].execute.taker_buffer = leadlag::TakerBufferConfig{
      .mode = leadlag::FeatureMode::kShadow,
      .entry_fixed_pct = 0.001,
      .normal_close_fixed_pct = 0.002,
      .exclude_from_cost_model = false,
      .source = leadlag::GeneratedParamSource::kGenerated,
  };
  return config;
}

leadlag::Config TwoPairSignalOnlyConfig() {
  leadlag::Config config = SignalOnlyConfig();
  leadlag::PairConfig second_pair = config.pairs.front();
  second_pair.symbol = "ETH_USDT";
  second_pair.symbol_id = 7;
  second_pair.lag_instrument.exchange_symbol = "ETH_USDT_GATE";
  config.pairs.push_back(second_pair);
  return config;
}

leadlag::RecoveryApplyResult SuccessfulRecoveryResult() {
  return leadlag::RecoveryApplyResult{
      .recovered = true,
      .position_match = true,
      .open_orders_resolved = true,
      .terminal_facts_resolved = true,
      .manual_intervention = false,
  };
}

leadlag::RecoveryApplyResult ManualRecoveryResult() {
  return leadlag::RecoveryApplyResult{
      .recovered = false,
      .position_match = false,
      .open_orders_resolved = false,
      .terminal_facts_resolved = false,
      .manual_intervention = true,
  };
}

aquila::config::StrategyConfig RuntimeConfig() {
  aquila::config::StrategyConfig config;
  config.name = "lead_lag";
  config.strategy_id = 4;
  config.order_capacity = 8;
  config.feedback.enabled = false;
  return config;
}

std::int64_t TestRealtimeNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::int64_t TickerEpochNs() noexcept {
  static const std::int64_t epoch_ns = TestRealtimeNowNs() - 10'000'000;
  return epoch_ns;
}

std::int64_t TickerLocalNs(std::int64_t local_ns) noexcept {
  return TickerEpochNs() + local_ns;
}

std::int64_t TickerExchangeNs(std::int64_t local_ns) noexcept {
  return TickerLocalNs(local_ns) - 10;
}

aquila::BookTicker Ticker(std::int32_t symbol_id, aquila::Exchange exchange,
                          std::int64_t local_ns, double bid_price,
                          double ask_price) {
  return aquila::BookTicker{
      .id = local_ns,
      .symbol_id = symbol_id,
      .exchange = exchange,
      .exchange_ns = TickerExchangeNs(local_ns),
      .local_ns = TickerLocalNs(local_ns),
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

void FeedOpenLongSignalForSymbol(leadlag::Strategy* strategy, ContextT* context,
                                 std::int32_t symbol_id) {
  strategy->OnBookTicker(
      Ticker(symbol_id, aquila::Exchange::kGate, 100, 101.57, 102.02),
      *context);
  strategy->OnBookTicker(
      Ticker(symbol_id, aquila::Exchange::kBinance, 100, 100.0, 101.0),
      *context);
  strategy->OnBookTicker(
      Ticker(symbol_id, aquila::Exchange::kBinance, 101, 112.0, 113.0),
      *context);
}

void FeedHugeOpenLongSignal(leadlag::Strategy* strategy, ContextT* context) {
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kGate, 100, 1.0157e64, 1.0202e64), *context);
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 1.0e64, 1.01e64), *context);
  strategy->OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 1.12e64, 1.13e64), *context);
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
                                          double quantity, double fill_price) {
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

aquila::OrderFeedbackEvent PartialFilledFeedback(std::uint64_t local_order_id,
                                                 double cumulative_quantity,
                                                 double left_quantity,
                                                 double fill_price) {
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
                                             double cumulative_quantity,
                                             double cancelled_quantity,
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
  EXPECT_EQ(diagnostics.event_ns, TickerExchangeNs(101));
  EXPECT_EQ(diagnostics.lead_raw.event_ns, TickerExchangeNs(101));
  EXPECT_DOUBLE_EQ(diagnostics.lead_raw.bid_price, 112.0);
  EXPECT_DOUBLE_EQ(diagnostics.lead_raw.ask_price, 113.0);
  EXPECT_EQ(diagnostics.lag.event_ns, TickerExchangeNs(100));
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
     ExternalModeFanoutOpenPlacesOneFullOrderPerRoute) {
  leadlag::Strategy strategy{SignalOnlyConfigWithFanout(4)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  const std::uint64_t parent_id = order_session.placed_orders[0].parent_id;
  ASSERT_NE(parent_id, 0U);
  std::vector<std::uint64_t> local_order_ids;
  for (std::size_t i = 0; i < order_session.placed_orders.size(); ++i) {
    const FakeOrderSession::CapturedOrder& order =
        order_session.placed_orders[i];
    local_order_ids.push_back(order.local_order_id);
    EXPECT_EQ(order.parent_id, parent_id);
    EXPECT_EQ(order.gateway_route_id, i);
    EXPECT_EQ(order.side, aquila::OrderSide::kBuy);
    EXPECT_FALSE(order.reduce_only);
    EXPECT_EQ(order.quantity, 9);
    EXPECT_EQ(order.price_text, "102.1");
  }
  EXPECT_NE(local_order_ids[0], local_order_ids[1]);
  EXPECT_NE(local_order_ids[1], local_order_ids[2]);
  EXPECT_NE(local_order_ids[2], local_order_ids[3]);
  EXPECT_EQ(strategy.last_signal_diagnostics().active_group_count, 1U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeParentIdIsGloballyUniqueAcrossSymbols) {
  leadlag::Strategy strategy{TwoPairSignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignalForSymbol(&strategy, &context, 3);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t first_parent_id =
      order_session.placed_orders.back().parent_id;
  ASSERT_NE(first_parent_id, 0U);

  FeedOpenLongSignalForSymbol(&strategy, &context, 7);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t second_parent_id =
      order_session.placed_orders.back().parent_id;
  ASSERT_NE(second_parent_id, 0U);
  EXPECT_NE(first_parent_id, second_parent_id);
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutOpenCapsOrdersAtAvailableOrderSessionRoutes) {
  leadlag::Strategy strategy{SignalOnlyConfigWithFanout(4)};
  FakeOrderSession order_session;
  order_session.max_order_session_fanout = 2;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  EXPECT_EQ(order_session.placed_orders[0].gateway_route_id, 0U);
  EXPECT_EQ(order_session.placed_orders[1].gateway_route_id, 1U);
  EXPECT_EQ(order_session.placed_orders[0].quantity, 9);
  EXPECT_EQ(order_session.placed_orders[1].quantity, 9);
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutOpenScansPastNotReadyRoutes) {
  leadlag::Strategy strategy{SignalOnlyConfigWithFanout(2)};
  FakeOrderSession order_session;
  order_session.max_order_session_fanout = 4;
  order_session.route_ready[0] = false;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  EXPECT_EQ(order_session.placed_orders[0].gateway_route_id, 1U);
  EXPECT_EQ(order_session.placed_orders[1].gateway_route_id, 2U);
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutOpenRefreshesRouteReadinessBeforeSelection) {
  leadlag::Strategy strategy{SignalOnlyConfigWithFanout(1)};
  FakeOrderSession order_session;
  order_session.max_order_session_fanout = 2;
  order_session.refresh_routes_marks_route0_not_ready = true;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  EXPECT_GE(order_session.refresh_route_calls, 1);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  EXPECT_EQ(order_session.placed_orders[0].gateway_route_id, 1U);
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutOpenRejectsWhenNoOrderRouteReady) {
  leadlag::Strategy strategy{SignalOnlyConfigWithFanout(4)};
  FakeOrderSession order_session;
  order_session.max_order_session_fanout = 4;
  order_session.route_ready.fill(false);
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  EXPECT_TRUE(order_session.placed_orders.empty());
  EXPECT_EQ(order_manager.order_count(), 0U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kOrderRouteNotReady);
}

TEST(LeadLagStrategyInterfaceTest, FanoutOpenRiskChecksTotalSubmittedNotional) {
  leadlag::Config config = SignalOnlyConfigWithFanout(4);
  config.risk.max_gross_notional = 2000.0;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  EXPECT_TRUE(order_session.placed_orders.empty());
  EXPECT_EQ(order_manager.order_count(), 0U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kRiskLimit);
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutPartialTerminalOpenCountsFilledChildForGrossRisk) {
  leadlag::Config config = TwoPairSignalOnlyConfig();
  for (leadlag::PairConfig& pair : config.pairs) {
    pair.execute.order_session_fanout = 4;
  }
  config.risk.max_gross_notional = 6600.0;
  config.risk.max_holding_position = 1'000.0;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignalForSymbol(&strategy, &context, 3);
  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  ApplyFeedback(
      &strategy, &order_manager, &context,
      FilledFeedback(order_session.placed_orders[0].local_order_id, 3, 102.1));

  FeedOpenLongSignalForSymbol(&strategy, &context, 7);

  EXPECT_EQ(order_session.placed_orders.size(), 4U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kRiskLimit);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModePlacesDecimalQuantityFromInstrumentStep) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].execute.open_notional = 10.21;
  config.pairs[0].lag_instrument.quantity_step = 0.1;
  config.pairs[0].lag_instrument.quantity_decimal_places = 1;
  config.pairs[0].lag_instrument.min_quantity = 0.1;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const FakeOrderSession::CapturedOrder& order =
      order_session.placed_orders.back();
  EXPECT_DOUBLE_EQ(order.quantity, 0.1);
  EXPECT_EQ(order.quantity_text, "0.1");
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRejectsOpenWhenLeadFreshnessExceedsLimit) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].max_lead_freshness_ms = 1;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  EXPECT_TRUE(order_session.placed_orders.empty());
  EXPECT_EQ(order_manager.order_count(), 0U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kMarketFreshness);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRejectsOpenWhenLagFreshnessExceedsLimit) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].max_lag_freshness_ms = 1;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  EXPECT_TRUE(order_session.placed_orders.empty());
  EXPECT_EQ(order_manager.order_count(), 0U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kMarketFreshness);
}

TEST(LeadLagStrategyInterfaceTest,
     ParallelLimitBlocksOpenAfterSignalTriggered) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategySignalTriggeredLogCaptureGuard signal_log_capture;
  StrategyOrderIntentRejectedLogCaptureGuard rejected_log_capture;

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  ASSERT_EQ(order_manager.order_count(), 1U);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 201, 105.0, 106.0),
                        context);
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 202, 170.0,
                               171.0),
                        context);

  EXPECT_EQ(order_session.placed_orders.size(), 1U);
  EXPECT_EQ(order_manager.order_count(), 1U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kParallelLimit);
  ASSERT_GE(g_signal_triggered_log_count, 2U);
  EXPECT_EQ(g_signal_triggered_logs[1].action,
            leadlag::SignalAction::kOpenLong);
  EXPECT_FALSE(g_signal_triggered_logs[1].reduce_only);
  ASSERT_EQ(g_order_intent_rejected_log_count, 1U);
  const leadlag::detail::StrategyOrderIntentRejectedLogRecordForTest& reject =
      g_order_intent_rejected_logs[0];
  EXPECT_EQ(reject.reason, "parallel_limit");
  EXPECT_EQ(reject.symbol, "BTC_USDT_GATE");
  EXPECT_EQ(reject.symbol_id, 3);
  EXPECT_EQ(reject.action, leadlag::SignalAction::kOpenLong);
  EXPECT_FALSE(reject.reduce_only);
}

TEST(LeadLagStrategyInterfaceTest, DriftGuardBlocksOpenAfterSignalTriggered) {
  leadlag::Strategy strategy{SignalOnlyConfigWithDriftGuard(0.001)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategySignalTriggeredLogCaptureGuard signal_log_capture;
  StrategyOrderIntentRejectedLogCaptureGuard rejected_log_capture;

  FeedOpenLongSignal(&strategy, &context);

  EXPECT_TRUE(order_session.placed_orders.empty());
  EXPECT_EQ(order_manager.order_count(), 0U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kDriftGuard);
  ASSERT_EQ(g_signal_triggered_log_count, 1U);
  EXPECT_EQ(g_signal_triggered_logs[0].action,
            leadlag::SignalAction::kOpenLong);
  EXPECT_FALSE(g_signal_triggered_logs[0].reduce_only);
  ASSERT_EQ(g_order_intent_rejected_log_count, 1U);
  const leadlag::detail::StrategyOrderIntentRejectedLogRecordForTest& reject =
      g_order_intent_rejected_logs[0];
  EXPECT_EQ(reject.reason, "drift_guard");
  EXPECT_EQ(reject.symbol, "BTC_USDT_GATE");
  EXPECT_EQ(reject.symbol_id, 3);
  EXPECT_EQ(reject.action, leadlag::SignalAction::kOpenLong);
  EXPECT_FALSE(reject.reduce_only);
}

TEST(LeadLagStrategyInterfaceTest,
     TriggeredSignalObserverSeesDriftGuardBlockedOpenBeforeReject) {
  leadlag::Strategy strategy{SignalOnlyConfigWithDriftGuard(0.001)};
  TriggeredSignalCapture capture;
  strategy.SetTriggeredSignalObserver(&capture, CaptureTriggeredSignal);
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(capture.count, 1U);
  EXPECT_EQ(capture.trigger_ticker.symbol_id, 3);
  EXPECT_TRUE(capture.decision.triggered);
  EXPECT_EQ(capture.decision.action, leadlag::SignalAction::kOpenLong);
  EXPECT_EQ(capture.decision.reject_reason, leadlag::SignalRejectReason::kNone);
  EXPECT_GT(capture.diagnostics.event_ns, 0);
  EXPECT_EQ(capture.diagnostics.lead_raw.id, 101);
  EXPECT_EQ(capture.diagnostics.lag.id, 100);
  EXPECT_TRUE(order_session.placed_orders.empty());
  EXPECT_EQ(order_manager.order_count(), 0U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kDriftGuard);
  EXPECT_FALSE(strategy.last_signal_diagnostics_valid());
}

TEST(LeadLagStrategyInterfaceTest, DisabledDriftGuardKeepsOpenPathUnchanged) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  EXPECT_EQ(order_manager.order_count(), 1U);
  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kOpenLong);
  EXPECT_EQ(decision.reject_reason, leadlag::SignalRejectReason::kNone);
}

TEST(LeadLagStrategyInterfaceTest,
     InitializationSkipsPairWhenOrderDecimalPlacesExceedBounds) {
  struct DecimalPlacesCase {
    std::int32_t price_decimal_places{0};
    std::int32_t quantity_decimal_places{0};
    double quantity_step{1.0};
    double min_quantity{1.0};
  };
  const DecimalPlacesCase cases[] = {
      {.price_decimal_places = 12, .quantity_decimal_places = 0},
      {.price_decimal_places = 0,
       .quantity_decimal_places = 12,
       .quantity_step = 0.000000000001,
       .min_quantity = 0.000000000001},
      {.price_decimal_places = 6,
       .quantity_decimal_places = 6,
       .quantity_step = 0.000001,
       .min_quantity = 0.000001},
  };

  for (const DecimalPlacesCase& test_case : cases) {
    leadlag::Config config = SignalOnlyConfig();
    config.pairs[0].lag_instrument.price_decimal_places =
        test_case.price_decimal_places;
    config.pairs[0].lag_instrument.quantity_step = test_case.quantity_step;
    config.pairs[0].lag_instrument.quantity_decimal_places =
        test_case.quantity_decimal_places;
    config.pairs[0].lag_instrument.min_quantity = test_case.min_quantity;
    leadlag::Strategy strategy{config};
    FakeOrderSession order_session;
    OrderManagerT order_manager{order_session, 8, 4};
    ContextT context{order_manager};

    FeedOpenLongSignal(&strategy, &context);

    EXPECT_FALSE(strategy.last_signal_decision().triggered)
        << "price_decimal_places=" << test_case.price_decimal_places
        << " quantity_decimal_places=" << test_case.quantity_decimal_places;
    EXPECT_TRUE(order_session.placed_orders.empty());
    EXPECT_EQ(order_manager.order_count(), 0U);
  }
}

TEST(LeadLagStrategyInterfaceTest,
     InitializationSkipsPairWhenOrderMetadataInvalid) {
  enum class InvalidOrderMetadataField : std::uint8_t {
    kPriceTick,
    kOpenNotional,
    kQuantityStep,
    kNotionalMultiplier,
  };
  const InvalidOrderMetadataField cases[] = {
      InvalidOrderMetadataField::kPriceTick,
      InvalidOrderMetadataField::kOpenNotional,
      InvalidOrderMetadataField::kQuantityStep,
      InvalidOrderMetadataField::kNotionalMultiplier,
  };

  for (const InvalidOrderMetadataField field : cases) {
    leadlag::Config config = SignalOnlyConfig();
    switch (field) {
      case InvalidOrderMetadataField::kPriceTick:
        config.pairs[0].lag_instrument.price_tick = 0.0;
        break;
      case InvalidOrderMetadataField::kOpenNotional:
        config.pairs[0].execute.open_notional = 0.0;
        break;
      case InvalidOrderMetadataField::kQuantityStep:
        config.pairs[0].lag_instrument.quantity_step = 0.0;
        break;
      case InvalidOrderMetadataField::kNotionalMultiplier:
        config.pairs[0].lag_instrument.notional_multiplier = 0.0;
        break;
    }
    leadlag::Strategy strategy{config};
    FakeOrderSession order_session;
    OrderManagerT order_manager{order_session, 8, 4};
    ContextT context{order_manager};

    FeedOpenLongSignal(&strategy, &context);

    EXPECT_FALSE(strategy.last_signal_decision().triggered);
    EXPECT_TRUE(order_session.placed_orders.empty());
    EXPECT_EQ(order_manager.order_count(), 0U);
  }
}

TEST(LeadLagStrategyInterfaceTest, LogsExternalOrderIntentBeforeSubmit) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategySignalTriggeredLogCaptureGuard signal_log_capture;
  StrategyOrderIntentLogCaptureGuard log_capture;

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  ASSERT_EQ(g_signal_triggered_log_count, 1U);
  const leadlag::detail::StrategySignalTriggeredLogRecordForTest& signal =
      g_signal_triggered_logs[0];
  EXPECT_EQ(signal.symbol, "BTC_USDT");
  EXPECT_EQ(signal.symbol_id, 3);
  EXPECT_EQ(signal.trigger_exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(signal.trigger_exchange_ns, TickerExchangeNs(101));
  EXPECT_EQ(signal.lead_exchange_ns, TickerExchangeNs(101));
  EXPECT_EQ(signal.lag_exchange_ns, TickerExchangeNs(100));
  EXPECT_EQ(signal.lead_local_ns, TickerLocalNs(101));
  EXPECT_EQ(signal.lag_local_ns, TickerLocalNs(100));
  EXPECT_EQ(signal.signal_lead_id, 101);
  EXPECT_EQ(signal.signal_lag_id, 100);
  EXPECT_EQ(signal.trigger_local_ns, TickerLocalNs(101));
  EXPECT_GT(signal.on_book_ticker_entry_ns, 0);
  EXPECT_GE(signal.signal_decision_ns, signal.on_book_ticker_entry_ns);
  EXPECT_EQ(signal.lead_freshness_ns,
            signal.signal_decision_ns - signal.lead_exchange_ns);
  EXPECT_EQ(signal.lag_freshness_ns,
            signal.signal_decision_ns - signal.lag_exchange_ns);
  EXPECT_EQ(signal.role, leadlag::PairRole::kLead);
  EXPECT_EQ(signal.action, leadlag::SignalAction::kOpenLong);
  EXPECT_EQ(signal.side, aquila::OrderSide::kBuy);
  ASSERT_EQ(g_order_intent_log_count, 1U);
  const leadlag::detail::StrategyOrderIntentLogRecordForTest& record =
      g_order_intent_logs[0];
  EXPECT_EQ(record.trigger_exchange_ns, signal.trigger_exchange_ns);
  EXPECT_EQ(record.lead_exchange_ns, signal.lead_exchange_ns);
  EXPECT_EQ(record.lag_exchange_ns, signal.lag_exchange_ns);
  EXPECT_EQ(record.lead_local_ns, signal.lead_local_ns);
  EXPECT_EQ(record.lag_local_ns, signal.lag_local_ns);
  EXPECT_EQ(record.signal_lead_id, signal.signal_lead_id);
  EXPECT_EQ(record.signal_lag_id, signal.signal_lag_id);
  EXPECT_EQ(record.lead_freshness_ns, signal.lead_freshness_ns);
  EXPECT_EQ(record.lag_freshness_ns, signal.lag_freshness_ns);
  EXPECT_EQ(record.max_lead_freshness_ns, kWideFreshnessGuardNs);
  EXPECT_EQ(record.max_lag_freshness_ns, kWideFreshnessGuardNs);
  EXPECT_TRUE(record.freshness_guard_pass);
  EXPECT_EQ(record.freshness_reject_reason,
            leadlag::FreshnessRejectReason::kNone);
  EXPECT_EQ(record.trigger_local_ns, signal.trigger_local_ns);
  EXPECT_EQ(record.on_book_ticker_entry_ns, signal.on_book_ticker_entry_ns);
  EXPECT_EQ(record.signal_decision_ns, signal.signal_decision_ns);
  EXPECT_EQ(record.symbol, "BTC_USDT_GATE");
  EXPECT_EQ(record.symbol_id, 3);
  EXPECT_EQ(record.action, leadlag::SignalAction::kOpenLong);
  EXPECT_EQ(record.side, aquila::OrderSide::kBuy);
  EXPECT_FALSE(record.reduce_only);
  EXPECT_EQ(record.position_id, 0U);
  EXPECT_EQ(record.quantity, 9);
  EXPECT_DOUBLE_EQ(record.raw_price, 102.02);
  EXPECT_DOUBLE_EQ(record.order_price, 102.1);
  EXPECT_DOUBLE_EQ(record.price, 102.1);
  EXPECT_DOUBLE_EQ(record.target_open_notional, 1000.0);
  EXPECT_DOUBLE_EQ(record.estimated_notional, 918.9);
  EXPECT_EQ(record.active_groups, 0U);
}

TEST(LeadLagStrategyInterfaceTest, LogsSignalDecisionWithReferenceShadowPrice) {
  leadlag::Strategy strategy{SignalOnlyConfigWithTakerBufferShadow()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategySignalDecisionLogCaptureGuard signal_decision_log_capture;
  StrategyOrderIntentLogCaptureGuard order_intent_log_capture;

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  ASSERT_EQ(g_order_intent_log_count, 1U);
  EXPECT_DOUBLE_EQ(g_order_intent_logs[0].raw_price, 102.02);
  EXPECT_DOUBLE_EQ(g_order_intent_logs[0].order_price, 102.1);

  ASSERT_EQ(g_signal_decision_log_count, 1U);
  const leadlag::detail::StrategySignalDecisionLogRecordForTest& record =
      g_signal_decision_logs[0];
  EXPECT_EQ(record.symbol, "BTC_USDT_GATE");
  EXPECT_EQ(record.symbol_id, 3);
  EXPECT_EQ(record.action, leadlag::SignalAction::kOpenLong);
  EXPECT_EQ(record.side, aquila::OrderSide::kBuy);
  EXPECT_EQ(record.decision, "sent");
  EXPECT_FALSE(record.reduce_only);
  EXPECT_DOUBLE_EQ(record.raw_price, 102.02);
  EXPECT_DOUBLE_EQ(record.current_order_price, 102.1);
  EXPECT_DOUBLE_EQ(record.reference_order_price, 102.2);
  EXPECT_DOUBLE_EQ(record.entry_buffer_pct, 0.001);
  EXPECT_DOUBLE_EQ(record.close_buffer_pct, 0.002);
  EXPECT_GT(record.lead_freshness_ns, 1'000'000);
  EXPECT_GT(record.lag_freshness_ns, 1'000'000);
}

TEST(LeadLagStrategyInterfaceTest,
     SkipsSignalDecisionLogWhenReferenceShadowDisabled) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategySignalDecisionLogCaptureGuard signal_decision_log_capture;
  StrategyOrderIntentLogCaptureGuard order_intent_log_capture;

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  EXPECT_EQ(g_order_intent_log_count, 1U);
  EXPECT_EQ(g_signal_decision_log_count, 0U);
}

TEST(LeadLagStrategyInterfaceTest, LogsExternalOrderSubmittedAfterSubmit) {
  leadlag::Strategy strategy{SignalOnlyConfigWithSlippage(3, 0)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategyOrderSubmittedLogCaptureGuard submitted_log_capture;

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const FakeOrderSession::CapturedOrder& order =
      order_session.placed_orders.back();
  ASSERT_EQ(g_order_submitted_log_count, 1U);
  const leadlag::detail::StrategyOrderSubmittedLogRecordForTest& record =
      g_order_submitted_logs[0];
  EXPECT_EQ(record.local_order_id, order.local_order_id);
  EXPECT_EQ(record.trigger_exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(record.trigger_symbol_id, 3);
  EXPECT_EQ(record.trigger_exchange_ns, TickerExchangeNs(101));
  EXPECT_EQ(record.lead_exchange_ns, TickerExchangeNs(101));
  EXPECT_EQ(record.lag_exchange_ns, TickerExchangeNs(100));
  EXPECT_EQ(record.lead_local_ns, TickerLocalNs(101));
  EXPECT_EQ(record.lag_local_ns, TickerLocalNs(100));
  EXPECT_EQ(record.signal_lead_id, 101);
  EXPECT_EQ(record.signal_lag_id, 100);
  EXPECT_EQ(record.lead_freshness_ns,
            record.signal_decision_ns - record.lead_exchange_ns);
  EXPECT_EQ(record.lag_freshness_ns,
            record.signal_decision_ns - record.lag_exchange_ns);
  EXPECT_EQ(record.max_lead_freshness_ns, kWideFreshnessGuardNs);
  EXPECT_EQ(record.max_lag_freshness_ns, kWideFreshnessGuardNs);
  EXPECT_TRUE(record.freshness_guard_pass);
  EXPECT_EQ(record.freshness_reject_reason,
            leadlag::FreshnessRejectReason::kNone);
  EXPECT_EQ(record.symbol, "BTC_USDT_GATE");
  EXPECT_EQ(record.symbol_id, 3);
  EXPECT_EQ(record.signal_role, leadlag::PairRole::kLead);
  EXPECT_EQ(record.order_role, "entry");
  EXPECT_EQ(record.action, leadlag::SignalAction::kOpenLong);
  EXPECT_EQ(record.side, aquila::OrderSide::kBuy);
  EXPECT_FALSE(record.reduce_only);
  EXPECT_NE(record.position_id, 0U);
  EXPECT_EQ(record.position_event, "kEntrySubmit");
  EXPECT_EQ(record.position_direction, leadlag::PositionDirection::kLong);
  EXPECT_EQ(record.entry_local_order_id, order.local_order_id);
  EXPECT_EQ(record.quantity, 9);
  EXPECT_EQ(record.quantity_text, "9");
  EXPECT_DOUBLE_EQ(record.raw_price, 102.02);
  EXPECT_DOUBLE_EQ(record.order_price, 102.4);
  EXPECT_EQ(record.price_text, "102.4");
  EXPECT_EQ(record.slippage_ticks, 3U);
  EXPECT_DOUBLE_EQ(record.price_tick, 0.1);
  EXPECT_DOUBLE_EQ(record.target_open_notional, 1000.0);
  EXPECT_DOUBLE_EQ(record.estimated_notional, 921.6);
  EXPECT_EQ(record.active_groups, 1U);
  EXPECT_EQ(record.place_status, aquila::core::OrderPlaceStatus::kOk);
}

TEST(LeadLagStrategyInterfaceTest, OrderResponseLogsCurrentLeadLagBboTiming) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategyOrderResponseLogCaptureGuard response_log_capture;

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t local_order_id =
      order_session.placed_orders[0].local_order_id;
  ASSERT_NE(local_order_id, 0U);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 201, 112.0, 113.0), context);
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 202, 101.57, 102.02),
                        context);
  ApplyResponse(&strategy, &order_manager, &context,
                aquila::core::OrderResponseEvent{
                    .kind = aquila::core::OrderResponseKind::kAck,
                    .local_order_id = local_order_id,
                    .exchange_order_id = 1001,
                    .local_receive_ns = 300,
                    .exchange_ns = 290,
                });

  ASSERT_EQ(g_order_response_log_count, 1U);
  const auto& log = g_order_response_logs[0];
  EXPECT_EQ(log.local_order_id, local_order_id);
  EXPECT_EQ(log.lead_exchange_ns, TickerExchangeNs(201));
  EXPECT_EQ(log.lag_exchange_ns, TickerExchangeNs(202));
  EXPECT_EQ(log.book_ticker_id_prefix, "ack");
  EXPECT_EQ(log.lead_book_ticker_id, 201);
  EXPECT_EQ(log.lag_book_ticker_id, 202);
}

TEST(LeadLagStrategyInterfaceTest,
     OrderFeedbackAndFinishedLogsCurrentLeadLagBboTiming) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategyOrderFeedbackLogCaptureGuard feedback_log_capture;
  StrategyOrderFinishedLogCaptureGuard finished_log_capture;

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t local_order_id =
      order_session.placed_orders[0].local_order_id;
  ASSERT_NE(local_order_id, 0U);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 201, 112.0, 113.0), context);
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 202, 101.57, 102.02),
                        context);
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(local_order_id, 7, 102.1));

  ASSERT_EQ(g_order_feedback_log_count, 1U);
  const auto& feedback_log = g_order_feedback_logs[0];
  EXPECT_EQ(feedback_log.local_order_id, local_order_id);
  EXPECT_EQ(feedback_log.lead_exchange_ns, TickerExchangeNs(201));
  EXPECT_EQ(feedback_log.lag_exchange_ns, TickerExchangeNs(202));
  EXPECT_EQ(feedback_log.book_ticker_id_prefix, "filled");
  EXPECT_EQ(feedback_log.lead_book_ticker_id, 201);
  EXPECT_EQ(feedback_log.lag_book_ticker_id, 202);

  ASSERT_EQ(g_order_finished_log_count, 1U);
  const auto& finished_log = g_order_finished_logs[0];
  EXPECT_EQ(finished_log.local_order_id, local_order_id);
  EXPECT_EQ(finished_log.lead_exchange_ns, TickerExchangeNs(201));
  EXPECT_EQ(finished_log.lag_exchange_ns, TickerExchangeNs(202));
}

TEST(LeadLagStrategyInterfaceTest, LogsCloseOrderSubmittedWithPositionId) {
  leadlag::Strategy strategy{SignalOnlyConfigWithSlippage(0, 3)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategyOrderSubmittedLogCaptureGuard submitted_log_capture;

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  ASSERT_EQ(g_order_submitted_log_count, 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  const std::uint64_t position_id = g_order_submitted_logs[0].position_id;
  ASSERT_NE(position_id, 0U);
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  ASSERT_EQ(g_order_submitted_log_count, 2U);
  const FakeOrderSession::CapturedOrder& close_order =
      order_session.placed_orders.back();
  const leadlag::detail::StrategyOrderSubmittedLogRecordForTest& record =
      g_order_submitted_logs[1];
  EXPECT_EQ(record.local_order_id, close_order.local_order_id);
  EXPECT_EQ(record.position_id, position_id);
  EXPECT_EQ(record.position_event, "kExitSubmit");
  EXPECT_EQ(record.position_direction, leadlag::PositionDirection::kLong);
  EXPECT_EQ(record.entry_local_order_id, open_order_id);
  EXPECT_EQ(record.order_role, "exit");
  EXPECT_TRUE(record.reduce_only);
}

TEST(LeadLagStrategyInterfaceTest, LogsOrderFinishedWithPositionId) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategyOrderSubmittedLogCaptureGuard submitted_log_capture;
  StrategyOrderFinishedLogCaptureGuard finished_log_capture;

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  ASSERT_EQ(g_order_submitted_log_count, 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  const std::uint64_t position_id = g_order_submitted_logs[0].position_id;
  ASSERT_NE(position_id, 0U);

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));

  ASSERT_EQ(g_order_finished_log_count, 1U);
  const leadlag::detail::StrategyOrderFinishedLogRecordForTest& record =
      g_order_finished_logs[0];
  EXPECT_EQ(record.local_order_id, open_order_id);
  EXPECT_EQ(record.position_id, position_id);
  EXPECT_EQ(record.position_direction, leadlag::PositionDirection::kLong);
  EXPECT_EQ(record.order_role, "entry");
  EXPECT_EQ(record.entry_local_order_id, open_order_id);
  EXPECT_GT(record.order_finished_local_ns, 0);
}

TEST(LeadLagStrategyInterfaceTest, StageBookTickerIdPrefixesMatchEventKinds) {
  EXPECT_EQ(leadlag::detail::OrderResponseBookTickerIdPrefix(
                aquila::core::OrderResponseKind::kAck),
            "ack");
  EXPECT_EQ(leadlag::detail::OrderResponseBookTickerIdPrefix(
                aquila::core::OrderResponseKind::kRejected),
            "rejected");
  EXPECT_EQ(leadlag::detail::OrderResponseBookTickerIdPrefix(
                aquila::core::OrderResponseKind::kUnknownResult),
            "unknown_result");
  EXPECT_EQ(leadlag::detail::OrderFeedbackBookTickerIdPrefix(
                aquila::OrderFeedbackKind::kCancelled),
            "cancelled");
  EXPECT_EQ(leadlag::detail::OrderFeedbackBookTickerIdPrefix(
                aquila::OrderFeedbackKind::kFilled),
            "filled");
  EXPECT_EQ(leadlag::detail::OrderFeedbackBookTickerIdPrefix(
                aquila::OrderFeedbackKind::kRejected),
            "rejected");
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeAppliesOpenSlippageToOpenLongOrder) {
  leadlag::Strategy strategy{SignalOnlyConfigWithSlippage(3, 0)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategyOrderIntentLogCaptureGuard log_capture;

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const FakeOrderSession::CapturedOrder& order =
      order_session.placed_orders.back();
  EXPECT_EQ(order.side, aquila::OrderSide::kBuy);
  EXPECT_FALSE(order.reduce_only);
  EXPECT_EQ(order.price_text, "102.4");
  EXPECT_EQ(order.quantity, 9);
  ASSERT_EQ(g_order_intent_log_count, 1U);
  EXPECT_DOUBLE_EQ(g_order_intent_logs[0].raw_price, 102.02);
  EXPECT_DOUBLE_EQ(g_order_intent_logs[0].order_price, 102.4);
  EXPECT_DOUBLE_EQ(g_order_intent_logs[0].price, 102.4);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeAppliesOpenSlippageToOpenShortOrder) {
  leadlag::Strategy strategy{SignalOnlyConfigWithSlippage(2, 0)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenShortSignal(&strategy, &context);

  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const FakeOrderSession::CapturedOrder& order =
      order_session.placed_orders.back();
  EXPECT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenShort);
  EXPECT_EQ(order.side, aquila::OrderSide::kSell);
  EXPECT_FALSE(order.reduce_only);
  EXPECT_EQ(order.price_text, "97.7");
  EXPECT_EQ(order.quantity, 10);
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

TEST(LeadLagStrategyInterfaceTest, CloseRejectsWhenNoOrderRouteReadyAndRetries) {
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
  order_session.route_ready.fill(false);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  EXPECT_EQ(order_session.placed_orders.size(), 1U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kOrderRouteNotReady);

  order_session.route_ready.fill(true);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 103, 99.0, 100.0), context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const FakeOrderSession::CapturedOrder& close_order =
      order_session.placed_orders.back();
  EXPECT_TRUE(close_order.reduce_only);
  EXPECT_EQ(close_order.quantity, 7);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeFanoutCloseUsesAggregatedFilledQuantity) {
  leadlag::Strategy strategy{SignalOnlyConfigWithFanout(4)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  const std::uint64_t parent_id = order_session.placed_orders[0].parent_id;
  const std::uint64_t first_open_order_id =
      order_session.placed_orders[0].local_order_id;
  const std::uint64_t second_open_order_id =
      order_session.placed_orders[1].local_order_id;
  const std::uint64_t third_open_order_id =
      order_session.placed_orders[2].local_order_id;
  const std::uint64_t fourth_open_order_id =
      order_session.placed_orders[3].local_order_id;

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(first_open_order_id, 3, 101.0));
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(second_open_order_id, 4, 104.0));
  ApplyFeedback(&strategy, &order_manager, &context,
                CancelledFeedback(third_open_order_id, 0, 9, 0.0));
  ApplyFeedback(&strategy, &order_manager, &context,
                CancelledFeedback(fourth_open_order_id, 0, 9, 0.0));

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  ASSERT_EQ(order_session.placed_orders.size(), 8U);
  for (std::size_t i = 4; i < 8; ++i) {
    const FakeOrderSession::CapturedOrder& close_order =
        order_session.placed_orders[i];
    EXPECT_EQ(close_order.parent_id, parent_id);
    EXPECT_EQ(close_order.gateway_route_id, i - 4);
    EXPECT_EQ(close_order.side, aquila::OrderSide::kSell);
    EXPECT_TRUE(close_order.reduce_only);
    EXPECT_EQ(close_order.quantity, 7);
    EXPECT_EQ(close_order.price_text, "101.5");
  }
  EXPECT_NEAR(strategy.last_signal_decision().trailing_price,
              ((3.0 * 101.0) + (4.0 * 104.0)) / 7.0, 1e-12);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeAppliesCloseSlippageToCloseLongOrder) {
  leadlag::Strategy strategy{SignalOnlyConfigWithSlippage(0, 3)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategyOrderIntentLogCaptureGuard log_capture;

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const FakeOrderSession::CapturedOrder& close_order =
      order_session.placed_orders.back();
  EXPECT_EQ(close_order.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(close_order.reduce_only);
  EXPECT_EQ(close_order.price_text, "101.2");
  EXPECT_EQ(close_order.quantity, 7);
  ASSERT_EQ(g_order_intent_log_count, 2U);
  EXPECT_DOUBLE_EQ(g_order_intent_logs[1].raw_price, 101.57);
  EXPECT_DOUBLE_EQ(g_order_intent_logs[1].order_price, 101.2);
  EXPECT_DOUBLE_EQ(g_order_intent_logs[1].price, 101.2);
}

TEST(LeadLagStrategyInterfaceTest,
     GlobalRiskMaxHoldingPositionBlocksNewOpenAcrossPairs) {
  leadlag::Config config = TwoPairSignalOnlyConfig();
  config.risk.max_gross_notional = 1'000'000.0;
  config.risk.max_holding_position = 9;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignalForSymbol(&strategy, &context, 3);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 9, 102.1));

  FeedOpenLongSignalForSymbol(&strategy, &context, 7);

  EXPECT_EQ(order_session.placed_orders.size(), 1U);
  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  EXPECT_FALSE(decision.triggered);
  EXPECT_EQ(decision.reject_reason, leadlag::SignalRejectReason::kRiskLimit);
}

TEST(LeadLagStrategyInterfaceTest,
     GlobalRiskMaxGrossNotionalBlocksNewOpenAcrossPairs) {
  leadlag::Config config = TwoPairSignalOnlyConfig();
  config.risk.max_gross_notional = 1'000.0;
  config.risk.max_holding_position = 1'000;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignalForSymbol(&strategy, &context, 3);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 9, 102.1));

  FeedOpenLongSignalForSymbol(&strategy, &context, 7);

  EXPECT_EQ(order_session.placed_orders.size(), 1U);
  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  EXPECT_FALSE(decision.triggered);
  EXPECT_EQ(decision.reject_reason, leadlag::SignalRejectReason::kRiskLimit);
}

TEST(LeadLagStrategyInterfaceTest, GlobalRiskLimitAllowsReduceOnlyClose) {
  leadlag::Config config = SignalOnlyConfig();
  config.risk.max_gross_notional = 920.0;
  config.risk.max_holding_position = 9;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 9, 102.1));

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const FakeOrderSession::CapturedOrder& close_order =
      order_session.placed_orders.back();
  EXPECT_TRUE(close_order.reduce_only);
  EXPECT_EQ(close_order.quantity, 9);
  EXPECT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kCloseLong);
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
     ExternalModeDoesNotRetryCloseWhenCloseRetryTimesZero) {
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

  EXPECT_EQ(order_session.placed_orders.size(), 2U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRetriesCloseRemainingAfterPartialTerminalCancelled) {
  leadlag::Strategy strategy{
      SignalOnlyConfigWithCloseRetry(/*close_slippage_ticks=*/3,
                                     /*stoploss_slippage_ticks=*/2,
                                     /*close_retry_times=*/1,
                                     /*close_retry_slippage_step_ticks=*/2)};
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
  EXPECT_EQ(retry_close.price_text, "101.0");
  EXPECT_EQ(retry_close.quantity, 4);

  ApplyFeedback(&strategy, &order_manager, &context,
                CancelledFeedback(retry_close.local_order_id, 0, 4, 0.0));
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 104, 98.0, 99.0),
                        context);
  EXPECT_EQ(order_session.placed_orders.size(), 3U);
}

TEST(LeadLagStrategyInterfaceTest, ExternalModeFanoutRetriesCloseRemaining) {
  leadlag::Strategy strategy{SignalOnlyConfigWithCloseRetryAndFanout(
      /*close_slippage_ticks=*/3, /*stoploss_slippage_ticks=*/2,
      /*close_retry_times=*/1, /*close_retry_slippage_step_ticks=*/2,
      /*fanout=*/4)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 24, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  const std::uint64_t open_parent_id = order_session.placed_orders[0].parent_id;
  ApplyFeedback(
      &strategy, &order_manager, &context,
      FilledFeedback(order_session.placed_orders[0].local_order_id, 7, 102.1));
  for (std::size_t i = 1; i < 4; ++i) {
    ApplyFeedback(
        &strategy, &order_manager, &context,
        CancelledFeedback(order_session.placed_orders[i].local_order_id, 0, 9,
                          0.0));
  }

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);
  ASSERT_EQ(order_session.placed_orders.size(), 8U);
  for (std::size_t i = 4; i < 8; ++i) {
    const double filled = i == 4 ? 3.0 : 0.0;
    const double cancelled = i == 4 ? 4.0 : 7.0;
    ApplyFeedback(
        &strategy, &order_manager, &context,
        CancelledFeedback(order_session.placed_orders[i].local_order_id, filled,
                          cancelled, 101.5));
  }

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 103, 99.0, 100.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 12U);
  for (std::size_t i = 8; i < 12; ++i) {
    const FakeOrderSession::CapturedOrder& retry_close =
        order_session.placed_orders[i];
    EXPECT_EQ(retry_close.parent_id, open_parent_id);
    EXPECT_EQ(retry_close.gateway_route_id, i - 8);
    EXPECT_EQ(retry_close.side, aquila::OrderSide::kSell);
    EXPECT_TRUE(retry_close.reduce_only);
    EXPECT_EQ(retry_close.price_text, "101.0");
    EXPECT_EQ(retry_close.quantity, 4);
  }
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeUsesFilledPositionQuantityForStoplossOrder) {
  leadlag::Strategy strategy{SignalOnlyConfigWithSlippage(0, 2)};
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
  EXPECT_EQ(stoploss_order.price_text, "94.3");
  EXPECT_EQ(stoploss_order.quantity, 11);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kStoplossLong);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeFanoutStoplossUsesAggregatedFilledQuantity) {
  leadlag::Config config = SignalOnlyConfigWithSlippage(0, 2);
  config.pairs[0].execute.order_session_fanout = 4;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  const std::uint64_t parent_id = order_session.placed_orders[0].parent_id;
  ApplyFeedback(
      &strategy, &order_manager, &context,
      FilledFeedback(order_session.placed_orders[0].local_order_id, 3, 102.1));
  ApplyFeedback(
      &strategy, &order_manager, &context,
      FilledFeedback(order_session.placed_orders[1].local_order_id, 4, 102.1));
  ApplyFeedback(&strategy, &order_manager, &context,
                CancelledFeedback(order_session.placed_orders[2].local_order_id,
                                  0, 9, 0.0));
  ApplyFeedback(&strategy, &order_manager, &context,
                CancelledFeedback(order_session.placed_orders[3].local_order_id,
                                  0, 9, 0.0));

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 95.0, 96.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 8U);
  for (std::size_t i = 4; i < 8; ++i) {
    const FakeOrderSession::CapturedOrder& stoploss_order =
        order_session.placed_orders[i];
    EXPECT_EQ(stoploss_order.parent_id, parent_id);
    EXPECT_EQ(stoploss_order.gateway_route_id, i - 4);
    EXPECT_EQ(stoploss_order.side, aquila::OrderSide::kSell);
    EXPECT_TRUE(stoploss_order.reduce_only);
    EXPECT_EQ(stoploss_order.price_text, "94.3");
    EXPECT_EQ(stoploss_order.quantity, 7);
  }
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutStoplossUsesKnownFillWhileSiblingOpenUnknown) {
  leadlag::Config config = SignalOnlyConfigWithSlippage(0, 2);
  config.pairs[0].execute.order_session_fanout = 2;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t parent_id = order_session.placed_orders[0].parent_id;
  const std::uint64_t first_open_order_id =
      order_session.placed_orders[0].local_order_id;
  const std::uint64_t second_open_order_id =
      order_session.placed_orders[1].local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(first_open_order_id, 3, 102.1));
  ApplyResponse(&strategy, &order_manager, &context,
                aquila::core::OrderResponseEvent{
                    .kind = aquila::core::OrderResponseKind::kUnknownResult,
                    .local_order_id = second_open_order_id,
                    .local_receive_ns = 601,
                });

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 95.0, 96.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  for (std::size_t i = 2; i < 4; ++i) {
    const FakeOrderSession::CapturedOrder& stoploss_order =
        order_session.placed_orders[i];
    EXPECT_EQ(stoploss_order.parent_id, parent_id);
    EXPECT_EQ(stoploss_order.gateway_route_id, i - 2);
    EXPECT_EQ(stoploss_order.side, aquila::OrderSide::kSell);
    EXPECT_TRUE(stoploss_order.reduce_only);
    EXPECT_EQ(stoploss_order.price_text, "94.3");
    EXPECT_EQ(stoploss_order.quantity, 3);
  }
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutLateOpenFillAfterCloseAllowsResidualNormalClose) {
  leadlag::Config config = SignalOnlyConfigWithFanout(2);
  config.pairs[0].execute.close_retry_times = 0;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t first_open_order_id =
      order_session.placed_orders[0].local_order_id;
  const std::uint64_t second_open_order_id =
      order_session.placed_orders[1].local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(first_open_order_id, 3, 102.1));
  ApplyResponse(&strategy, &order_manager, &context,
                aquila::core::OrderResponseEvent{
                    .kind = aquila::core::OrderResponseKind::kUnknownResult,
                    .local_order_id = second_open_order_id,
                    .local_receive_ns = 601,
                });

  order_session.route_ready.fill(false);
  order_session.route_ready[0] = true;
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  ASSERT_EQ(order_session.placed_orders.size(), 3U);
  const FakeOrderSession::CapturedOrder& first_close =
      order_session.placed_orders.back();
  ASSERT_TRUE(first_close.reduce_only);
  EXPECT_EQ(first_close.quantity, 3);
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(first_close.local_order_id, 3, 101.5));

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(second_open_order_id, 4, 104.0));
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 103, 99.0, 100.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  const FakeOrderSession::CapturedOrder& residual_close =
      order_session.placed_orders.back();
  EXPECT_NE(residual_close.local_order_id, first_close.local_order_id);
  EXPECT_EQ(residual_close.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(residual_close.reduce_only);
  EXPECT_EQ(residual_close.quantity, 4);
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutLateOpenFillBeforeCloseFinishAllowsResidualNormalClose) {
  leadlag::Config config = SignalOnlyConfigWithFanout(2);
  config.pairs[0].execute.close_retry_times = 0;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t first_open_order_id =
      order_session.placed_orders[0].local_order_id;
  const std::uint64_t second_open_order_id =
      order_session.placed_orders[1].local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(first_open_order_id, 3, 102.1));
  ApplyResponse(&strategy, &order_manager, &context,
                aquila::core::OrderResponseEvent{
                    .kind = aquila::core::OrderResponseKind::kUnknownResult,
                    .local_order_id = second_open_order_id,
                    .local_receive_ns = 601,
                });

  order_session.route_ready.fill(false);
  order_session.route_ready[0] = true;
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  ASSERT_EQ(order_session.placed_orders.size(), 3U);
  const FakeOrderSession::CapturedOrder& first_close =
      order_session.placed_orders.back();
  ASSERT_TRUE(first_close.reduce_only);
  EXPECT_EQ(first_close.quantity, 3);

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(second_open_order_id, 4, 104.0));
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(first_close.local_order_id, 3, 101.5));
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 103, 99.0, 100.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  const FakeOrderSession::CapturedOrder& residual_close =
      order_session.placed_orders.back();
  EXPECT_NE(residual_close.local_order_id, first_close.local_order_id);
  EXPECT_EQ(residual_close.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(residual_close.reduce_only);
  EXPECT_EQ(residual_close.quantity, 4);
}

TEST(LeadLagStrategyInterfaceTest,
     StoplossRejectsWhenNoOrderRouteReadyAndRetries) {
  leadlag::Strategy strategy{SignalOnlyConfigWithSlippage(0, 2)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 11, 102.1));
  order_session.route_ready.fill(false);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 95.0, 96.0),
                        context);

  EXPECT_EQ(order_session.placed_orders.size(), 1U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kOrderRouteNotReady);

  order_session.route_ready.fill(true);
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 103, 94.9, 95.9),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const FakeOrderSession::CapturedOrder& stoploss_order =
      order_session.placed_orders.back();
  EXPECT_TRUE(stoploss_order.reduce_only);
  EXPECT_EQ(stoploss_order.quantity, 11);
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
  leadlag::Strategy strategy{
      SignalOnlyConfigWithCloseRetry(/*close_slippage_ticks=*/3,
                                     /*stoploss_slippage_ticks=*/2,
                                     /*close_retry_times=*/1,
                                     /*close_retry_slippage_step_ticks=*/2)};
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
  EXPECT_EQ(retry_stoploss.price_text, "89.3");
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
     ExternalModeUnknownOpenResultWaitsForLateFillFeedback) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;

  ApplyResponse(&strategy, &order_manager, &context,
                aquila::core::OrderResponseEvent{
                    .kind = aquila::core::OrderResponseKind::kUnknownResult,
                    .local_order_id = open_order_id,
                    .local_receive_ns = 1234,
                    .exchange_ns = 1200,
                });

  const aquila::core::StrategyOrder* pending_order =
      context.FindOrder(open_order_id);
  ASSERT_NE(pending_order, nullptr);
  EXPECT_EQ(pending_order->status, aquila::core::OrderStatus::kSent);
  EXPECT_FALSE(pending_order->is_finished);
  EXPECT_EQ(order_manager.order_count(), 1U);
  EXPECT_TRUE(strategy.needs_reconcile());
  EXPECT_TRUE(strategy.new_entries_paused());

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));

  EXPECT_EQ(context.FindOrder(open_order_id), nullptr);
  EXPECT_EQ(order_manager.order_count(), 0U);

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const FakeOrderSession::CapturedOrder& close_order =
      order_session.placed_orders.back();
  EXPECT_EQ(close_order.side, aquila::OrderSide::kSell);
  EXPECT_TRUE(close_order.reduce_only);
  EXPECT_EQ(close_order.quantity, 7);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeUnknownOpenResultLateTerminalFeedbackResumesNewEntries) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t open_order_id =
      order_session.placed_orders.back().local_order_id;

  ApplyResponse(&strategy, &order_manager, &context,
                aquila::core::OrderResponseEvent{
                    .kind = aquila::core::OrderResponseKind::kUnknownResult,
                    .local_order_id = open_order_id,
                    .local_receive_ns = 1234,
                    .exchange_ns = 1200,
                });

  EXPECT_TRUE(strategy.needs_reconcile());
  EXPECT_TRUE(strategy.new_entries_paused());

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(open_order_id, 7, 102.1));

  EXPECT_FALSE(strategy.needs_reconcile());
  EXPECT_FALSE(strategy.new_entries_paused());

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);
  ASSERT_EQ(order_session.placed_orders.size(), 2U);
  const std::uint64_t close_order_id =
      order_session.placed_orders.back().local_order_id;
  EXPECT_TRUE(order_session.placed_orders.back().reduce_only);

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(close_order_id, 7, 101.5));

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 200, 101.57, 102.02),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 200, 100.0, 101.0), context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 201, 112.0, 113.0), context);
  ASSERT_EQ(order_session.placed_orders.size(), 3U);
  EXPECT_FALSE(order_session.placed_orders.back().reduce_only);
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutUnknownOpenResultWaitsForEveryUnknownChild) {
  leadlag::Strategy strategy{SignalOnlyConfigWithFanout(4)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 16, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  const std::uint64_t first_unknown_id =
      order_session.placed_orders[0].local_order_id;
  const std::uint64_t second_unknown_id =
      order_session.placed_orders[1].local_order_id;

  ApplyResponse(&strategy, &order_manager, &context,
                aquila::core::OrderResponseEvent{
                    .kind = aquila::core::OrderResponseKind::kUnknownResult,
                    .local_order_id = first_unknown_id,
                    .local_receive_ns = 1234,
                    .exchange_ns = 1200,
                });
  ApplyResponse(&strategy, &order_manager, &context,
                aquila::core::OrderResponseEvent{
                    .kind = aquila::core::OrderResponseKind::kUnknownResult,
                    .local_order_id = second_unknown_id,
                    .local_receive_ns = 1235,
                    .exchange_ns = 1201,
                });
  EXPECT_TRUE(strategy.needs_reconcile());
  EXPECT_TRUE(strategy.new_entries_paused());

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(first_unknown_id, 3, 102.1));

  EXPECT_TRUE(strategy.needs_reconcile());
  EXPECT_TRUE(strategy.new_entries_paused());

  ApplyFeedback(&strategy, &order_manager, &context,
                FilledFeedback(second_unknown_id, 4, 102.1));
  ApplyFeedback(&strategy, &order_manager, &context,
                CancelledFeedback(order_session.placed_orders[2].local_order_id,
                                  0, 9, 0.0));
  ApplyFeedback(&strategy, &order_manager, &context,
                CancelledFeedback(order_session.placed_orders[3].local_order_id,
                                  0, 9, 0.0));

  EXPECT_FALSE(strategy.needs_reconcile());
  EXPECT_FALSE(strategy.new_entries_paused());
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeRejectedCloseReturnsHoldAndCanRetry) {
  leadlag::Strategy strategy{
      SignalOnlyConfigWithCloseRetry(/*close_slippage_ticks=*/3,
                                     /*stoploss_slippage_ticks=*/2,
                                     /*close_retry_times=*/1,
                                     /*close_retry_slippage_step_ticks=*/2)};
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
  EXPECT_EQ(retry_close.price_text, "101.0");
  EXPECT_EQ(retry_close.quantity, 7);
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutSyncRejectedCloseAdvancesRetrySlippage) {
  leadlag::Strategy strategy{SignalOnlyConfigWithCloseRetryAndFanout(
      /*close_slippage_ticks=*/3, /*stoploss_slippage_ticks=*/2,
      /*close_retry_times=*/1, /*close_retry_slippage_step_ticks=*/2,
      /*fanout=*/4)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 24, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  ApplyFeedback(
      &strategy, &order_manager, &context,
      FilledFeedback(order_session.placed_orders[0].local_order_id, 7, 102.1));
  for (std::size_t i = 1; i < 4; ++i) {
    ApplyFeedback(
        &strategy, &order_manager, &context,
        CancelledFeedback(order_session.placed_orders[i].local_order_id, 0, 9,
                          0.0));
  }

  order_session.next_place_status = FakeOrderSession::SendStatus::kRejected;
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);
  ASSERT_EQ(order_session.placed_orders.size(), 8U);
  EXPECT_EQ(order_manager.order_count(), 0U);

  order_session.next_place_status = FakeOrderSession::SendStatus::kOk;
  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 103, 99.0, 100.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 12U);
  for (std::size_t i = 8; i < 12; ++i) {
    const FakeOrderSession::CapturedOrder& retry_close =
        order_session.placed_orders[i];
    EXPECT_TRUE(retry_close.reduce_only);
    EXPECT_EQ(retry_close.price_text, "101.0");
    EXPECT_EQ(retry_close.quantity, 7);
  }
}

TEST(LeadLagStrategyInterfaceTest,
     FanoutMixedSyncRejectedCloseRetriesRemainingOnce) {
  leadlag::Strategy strategy{SignalOnlyConfigWithCloseRetryAndFanout(
      /*close_slippage_ticks=*/3, /*stoploss_slippage_ticks=*/2,
      /*close_retry_times=*/1, /*close_retry_slippage_step_ticks=*/2,
      /*fanout=*/4)};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 24, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 4U);
  ApplyFeedback(
      &strategy, &order_manager, &context,
      FilledFeedback(order_session.placed_orders[0].local_order_id, 7, 102.1));
  for (std::size_t i = 1; i < 4; ++i) {
    ApplyFeedback(
        &strategy, &order_manager, &context,
        CancelledFeedback(order_session.placed_orders[i].local_order_id, 0, 9,
                          0.0));
  }

  order_session.reject_next_place_count = 2;
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);
  ASSERT_EQ(order_session.placed_orders.size(), 8U);
  EXPECT_EQ(order_manager.order_count(), 2U);

  ApplyFeedback(
      &strategy, &order_manager, &context,
      CancelledFeedback(order_session.placed_orders[6].local_order_id, 3, 4,
                        101.5));
  ApplyFeedback(
      &strategy, &order_manager, &context,
      CancelledFeedback(order_session.placed_orders[7].local_order_id, 0, 7,
                        0.0));

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 103, 99.0, 100.0),
                        context);

  ASSERT_EQ(order_session.placed_orders.size(), 12U);
  for (std::size_t i = 8; i < 12; ++i) {
    const FakeOrderSession::CapturedOrder& retry_close =
        order_session.placed_orders[i];
    EXPECT_TRUE(retry_close.reduce_only);
    EXPECT_EQ(retry_close.price_text, "101.0");
    EXPECT_EQ(retry_close.quantity, 4);
  }
}

TEST(LeadLagStrategyInterfaceTest,
     RetiredRejectedOrdersDoNotAllocatePriceTextStorageInOrderPath) {
  leadlag::Config config = SignalOnlyConfig();
  {
    // Warm lazy logging/formatting state before measuring the order path.
    leadlag::Strategy warm_strategy{config};
    FakeOrderSession warm_session;
    warm_session.next_place_status = FakeOrderSession::SendStatus::kRejected;
    warm_session.capture_price_text = false;
    warm_session.placed_orders.reserve(1);
    OrderManagerT warm_manager{warm_session, 8, 4};
    ContextT warm_context{warm_manager};

    warm_strategy.OnBookTicker(
        Ticker(3, aquila::Exchange::kGate, 100, 101.57, 102.02),
        warm_context);
    warm_strategy.OnBookTicker(
        Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0),
        warm_context);
    warm_strategy.OnBookTicker(
        Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0),
        warm_context);
    ASSERT_EQ(warm_session.placed_orders.size(), 1U);
  }
  FlushStrategyLogging();

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
  FlushStrategyLogging();

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

TEST(LeadLagStrategyInterfaceTest,
     InitializationSkipsPairWhenPriceDecimalPlacesInvalid) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].lag_instrument.price_decimal_places = 128;
  leadlag::Strategy strategy{config};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_TRUE(order_session.placed_orders.empty());
  EXPECT_EQ(order_manager.order_count(), 0U);
}

TEST(LeadLagStrategyInterfaceTest,
     ExternalModeSkipsOrderWhenPriceUnitsOverflow) {
  leadlag::Config config = SignalOnlyConfig();
  config.pairs[0].lag_instrument.price_tick = 1.0;
  config.pairs[0].lag_instrument.price_decimal_places = 11;
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
  leadlag::Strategy strategy{SignalOnlyConfigWithSlippage(0, 4)};
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
  EXPECT_EQ(stoploss_order.price_text, "104.0");
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

TEST(LeadLagStrategyInterfaceTest, DriftGuardDoesNotBlockNormalClose) {
  leadlag::Strategy strategy{
      SignalOnlyConfigWithDriftGuard(0.10),
      leadlag::StrategyOptions{
          .position_accounting =
              leadlag::PositionAccountingMode::kSyntheticSignals,
      }};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_TRUE(strategy.last_signal_decision().triggered)
      << static_cast<int>(strategy.last_signal_decision().reject_reason);
  ASSERT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 102, 80.0, 81.0),
                        context);

  const leadlag::SignalDecision& close = strategy.last_signal_decision();
  ASSERT_TRUE(close.triggered) << static_cast<int>(close.reject_reason);
  EXPECT_EQ(close.action, leadlag::SignalAction::kCloseLong);
  EXPECT_EQ(close.reject_reason, leadlag::SignalRejectReason::kNone);
  EXPECT_TRUE(close.intent.reduce_only);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 103, 70.0, 71.0),
                        context);

  EXPECT_FALSE(strategy.last_signal_decision().triggered);
}

TEST(LeadLagStrategyInterfaceTest, DriftGuardDoesNotBlockStoploss) {
  leadlag::Strategy strategy{
      SignalOnlyConfigWithDriftGuard(0.10),
      leadlag::StrategyOptions{
          .position_accounting =
              leadlag::PositionAccountingMode::kSyntheticSignals,
      }};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_TRUE(strategy.last_signal_decision().triggered)
      << static_cast<int>(strategy.last_signal_decision().reject_reason);
  ASSERT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 102, 95.0, 96.0),
                        context);

  const leadlag::SignalDecision& stoploss = strategy.last_signal_decision();
  ASSERT_TRUE(stoploss.triggered) << static_cast<int>(stoploss.reject_reason);
  EXPECT_EQ(stoploss.action, leadlag::SignalAction::kStoplossLong);
  EXPECT_EQ(stoploss.reject_reason, leadlag::SignalRejectReason::kNone);
  EXPECT_TRUE(stoploss.intent.reduce_only);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 103, 94.0, 95.0),
                        context);

  EXPECT_FALSE(strategy.last_signal_decision().triggered);
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

#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)

std::vector<leadlag::MarketCalcRow> g_market_calc_rows;

void CaptureMarketCalcRow(void*, const leadlag::MarketCalcRow& row) noexcept {
  g_market_calc_rows.push_back(row);
}

class MarketCalcRowCaptureGuard {
 public:
  explicit MarketCalcRowCaptureGuard(leadlag::Strategy* strategy) noexcept
      : strategy_(strategy) {
    g_market_calc_rows.clear();
    strategy_->SetMarketCalcObserver(nullptr, CaptureMarketCalcRow);
  }

  ~MarketCalcRowCaptureGuard() noexcept {
    if (strategy_ != nullptr) {
      strategy_->SetMarketCalcObserver(nullptr, nullptr);
    }
    g_market_calc_rows.clear();
  }

  MarketCalcRowCaptureGuard(const MarketCalcRowCaptureGuard&) = delete;
  MarketCalcRowCaptureGuard& operator=(const MarketCalcRowCaptureGuard&) =
      delete;

 private:
  leadlag::Strategy* strategy_{nullptr};
};

TEST(LeadLagStrategyInterfaceTest,
     MarketCalcDiagnosticModeEmitsRowsWithoutSignalsOrOrders) {
  leadlag::Strategy strategy{
      SignalOnlyConfig(),
      leadlag::StrategyOptions{
          .position_accounting =
              leadlag::PositionAccountingMode::kSyntheticSignals,
          .market_calc_diagnostics_only = true,
      }};
  MarketCalcRowCaptureGuard capture{&strategy};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignal(&strategy, &context);

  ASSERT_EQ(g_market_calc_rows.size(), 3U);
  EXPECT_EQ(g_market_calc_rows[0].row_index, 1U);
  EXPECT_EQ(g_market_calc_rows[0].role, leadlag::PairRole::kLag);
  EXPECT_EQ(g_market_calc_rows[0].symbol_id, 3);
  EXPECT_EQ(g_market_calc_rows[0].book_ticker_id, 100);
  EXPECT_FALSE(g_market_calc_rows[0].both_sides_valid);
  EXPECT_FALSE(g_market_calc_rows[0].active);
  EXPECT_TRUE(std::isnan(g_market_calc_rows[0].lead_bid));
  EXPECT_DOUBLE_EQ(g_market_calc_rows[0].lag_bid, 101.57);
  EXPECT_TRUE(std::isnan(g_market_calc_rows[0].drift_mean));

  const leadlag::MarketCalcRow& active_lead = g_market_calc_rows.back();
  EXPECT_EQ(active_lead.row_index, 3U);
  EXPECT_EQ(active_lead.role, leadlag::PairRole::kLead);
  EXPECT_TRUE(active_lead.both_sides_valid);
  EXPECT_TRUE(active_lead.active);
  EXPECT_TRUE(active_lead.price_changed);
  EXPECT_DOUBLE_EQ(active_lead.lead_bid, 112.0);
  EXPECT_DOUBLE_EQ(active_lead.lag_ask, 102.02);
  EXPECT_FALSE(std::isnan(active_lead.drift_mean));
  EXPECT_FALSE(std::isnan(active_lead.drifted_lead_bid));
  EXPECT_FALSE(std::isnan(active_lead.long_lead_move));
  EXPECT_FALSE(std::isnan(active_lead.long_required_edge));

  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_FALSE(strategy.last_signal_diagnostics_valid());
  EXPECT_TRUE(order_session.placed_orders.empty());

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 102, 100.0, 101.0), context);

  ASSERT_EQ(g_market_calc_rows.size(), 4U);
  EXPECT_EQ(g_market_calc_rows.back().role, leadlag::PairRole::kLead);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_TRUE(order_session.placed_orders.empty());
}

#endif

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
  EXPECT_EQ(strategy.recovery_state(),
            leadlag::RecoveryState::kDegradedNeedsReconcile);
  EXPECT_TRUE(strategy.needs_reconcile());
  EXPECT_TRUE(strategy.new_entries_paused());

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);

  const leadlag::SignalDecision& decision = strategy.last_signal_decision();
  EXPECT_FALSE(decision.triggered);
  EXPECT_EQ(decision.reject_reason, leadlag::SignalRejectReason::kDegraded);
  EXPECT_TRUE(strategy.degraded());
  EXPECT_TRUE(order_session.placed_orders.empty());
}

TEST(LeadLagStrategyInterfaceTest,
     BeginReconcileMovesDegradedStateAndKeepsOpenSignalsPaused) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kContinuityLost,
      },
      context);

  ASSERT_TRUE(strategy.BeginReconcile());
  EXPECT_EQ(strategy.recovery_state(), leadlag::RecoveryState::kReconciling);
  EXPECT_TRUE(strategy.needs_reconcile());
  EXPECT_TRUE(strategy.new_entries_paused());

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);

  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kDegraded);
  EXPECT_TRUE(order_session.placed_orders.empty());
}

TEST(LeadLagStrategyInterfaceTest,
     ExecutionRecoverySuccessWithoutBeginReconcileDoesNotRestoreNormal) {
  leadlag::ExecutionState execution;
  execution.Init(/*parallel=*/1);
  execution.OnFeedbackContinuityLost(aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kContinuityLost,
  });

  ASSERT_EQ(execution.recovery_state(),
            leadlag::RecoveryState::kDegradedNeedsReconcile);

  EXPECT_FALSE(execution.ApplyRecoveryResult(SuccessfulRecoveryResult()));
  EXPECT_EQ(execution.recovery_state(),
            leadlag::RecoveryState::kDegradedNeedsReconcile);
  EXPECT_TRUE(execution.needs_reconcile());
  EXPECT_TRUE(execution.new_entries_paused());
}

TEST(LeadLagStrategyInterfaceTest,
     ExecutionManualInterventionIsStickyAcrossSuccessfulRecoveryResult) {
  leadlag::ExecutionState execution;
  execution.Init(/*parallel=*/1);
  execution.OnFeedbackContinuityLost(aquila::OrderFeedbackEvent{
      .kind = aquila::OrderFeedbackKind::kContinuityLost,
  });
  ASSERT_TRUE(execution.BeginReconcile());
  ASSERT_FALSE(execution.ApplyRecoveryResult(ManualRecoveryResult()));
  ASSERT_EQ(execution.recovery_state(),
            leadlag::RecoveryState::kManualIntervention);

  EXPECT_FALSE(execution.ApplyRecoveryResult(SuccessfulRecoveryResult()));
  EXPECT_EQ(execution.recovery_state(),
            leadlag::RecoveryState::kManualIntervention);
  EXPECT_TRUE(execution.manual_intervention());
  EXPECT_TRUE(execution.needs_reconcile());
  EXPECT_TRUE(execution.new_entries_paused());
}

TEST(LeadLagStrategyInterfaceTest,
     FailedRecoveryResultRequiresManualInterventionAndCannotClearReconcile) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kContinuityLost,
      },
      context);
  ASSERT_TRUE(strategy.BeginReconcile());

  EXPECT_FALSE(strategy.ApplyRecoveryResult(ManualRecoveryResult()));

  EXPECT_EQ(strategy.recovery_state(),
            leadlag::RecoveryState::kManualIntervention);
  EXPECT_TRUE(strategy.manual_intervention());
  EXPECT_TRUE(strategy.needs_reconcile());
  EXPECT_TRUE(strategy.new_entries_paused());
}

TEST(LeadLagStrategyInterfaceTest,
     SuccessfulRecoveryResultClearsReconcileAndAllowsLaterOpenSignal) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 100, 101.5, 102.0),
                        context);
  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0), context);
  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kContinuityLost,
      },
      context);
  ASSERT_TRUE(strategy.BeginReconcile());

  ASSERT_TRUE(strategy.ApplyRecoveryResult(SuccessfulRecoveryResult()));
  EXPECT_EQ(strategy.recovery_state(), leadlag::RecoveryState::kNormal);
  EXPECT_FALSE(strategy.needs_reconcile());
  EXPECT_FALSE(strategy.manual_intervention());
  EXPECT_FALSE(strategy.new_entries_paused());

  strategy.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 101, 112.0, 113.0), context);

  EXPECT_TRUE(strategy.last_signal_decision().triggered)
      << static_cast<int>(strategy.last_signal_decision().reject_reason);
  EXPECT_EQ(strategy.last_signal_decision().action,
            leadlag::SignalAction::kOpenLong);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
}

TEST(LeadLagStrategyInterfaceTest,
     StrategyWideRecoverySuccessWithoutBeginReconcileKeepsMultiPairPaused) {
  leadlag::Strategy strategy{TwoPairSignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kContinuityLost,
      },
      context);
  ASSERT_EQ(strategy.recovery_state(),
            leadlag::RecoveryState::kDegradedNeedsReconcile);

  EXPECT_FALSE(strategy.ApplyRecoveryResult(SuccessfulRecoveryResult()));
  EXPECT_EQ(strategy.recovery_state(),
            leadlag::RecoveryState::kDegradedNeedsReconcile);
  EXPECT_TRUE(strategy.needs_reconcile());
  EXPECT_TRUE(strategy.new_entries_paused());
}

TEST(LeadLagStrategyInterfaceTest,
     SinglePairUnknownResultRecoveryDoesNotRequireOtherPairsReconciling) {
  leadlag::Strategy strategy{TwoPairSignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  FeedOpenLongSignalForSymbol(&strategy, &context, 3);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  const std::uint64_t local_order_id =
      order_session.placed_orders.back().local_order_id;
  ApplyResponse(&strategy, &order_manager, &context,
                aquila::core::OrderResponseEvent{
                    .kind = aquila::core::OrderResponseKind::kUnknownResult,
                    .local_order_id = local_order_id,
                    .local_receive_ns = 701,
                });
  ASSERT_TRUE(strategy.needs_reconcile());
  ASSERT_TRUE(strategy.BeginReconcile());

  EXPECT_TRUE(strategy.ApplyRecoveryResult(SuccessfulRecoveryResult()));

  EXPECT_EQ(strategy.recovery_state(), leadlag::RecoveryState::kNormal);
  EXPECT_FALSE(strategy.needs_reconcile());
  EXPECT_FALSE(strategy.new_entries_paused());
}

TEST(LeadLagStrategyInterfaceTest,
     StrategyWideRecoverySuccessDoesNotClearManualMultiPairRuntime) {
  leadlag::Strategy strategy{TwoPairSignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};

  strategy.OnOrderFeedback(
      aquila::OrderFeedbackEvent{
          .kind = aquila::OrderFeedbackKind::kContinuityLost,
      },
      context);
  ASSERT_TRUE(strategy.BeginReconcile());
  ASSERT_FALSE(strategy.ApplyRecoveryResult(ManualRecoveryResult()));
  ASSERT_EQ(strategy.recovery_state(),
            leadlag::RecoveryState::kManualIntervention);

  EXPECT_FALSE(strategy.ApplyRecoveryResult(SuccessfulRecoveryResult()));
  EXPECT_EQ(strategy.recovery_state(),
            leadlag::RecoveryState::kManualIntervention);
  EXPECT_TRUE(strategy.manual_intervention());
  EXPECT_TRUE(strategy.needs_reconcile());
  EXPECT_TRUE(strategy.new_entries_paused());
}

}  // namespace
