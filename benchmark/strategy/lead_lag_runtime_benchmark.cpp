#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmark/strategy/lead_lag_benchmark_support.h"
#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/config/strategy_config.h"
#include "core/market_data/types.h"
#include "core/trading/order_types.h"
#include "core/trading/trading_runtime.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/strategy.h"

namespace aquila::strategy::leadlag {
namespace {

constexpr std::uint8_t kStrategyId = 4;
constexpr std::int32_t kSymbolId = 3;
constexpr std::size_t kOrderCapacity = 8;
constexpr std::size_t kLatencyIterations = 4096;

struct SharedOrderSessionState {
  std::uint64_t place_calls{0};
  std::uint64_t cancel_calls{0};
  std::uint64_t last_place_local_order_id{0};
  double last_quantity{0.0};
  OrderSide last_side{OrderSide::kBuy};
  bool last_reduce_only{false};
};

struct BenchmarkOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  explicit BenchmarkOrderSession(SharedOrderSessionState* shared_state)
      : state(shared_state) {}

  [[nodiscard]] bool Start() noexcept {
    running = true;
    return true;
  }

  void Stop() noexcept {
    running = false;
  }

  [[nodiscard]] bool Ready() const noexcept {
    return true;
  }

  [[nodiscard]] bool Running() const noexcept {
    return running;
  }

  SendResult PlaceOrder(const core::OrderPlaceRequest& request) noexcept {
    if (state != nullptr) {
      ++state->place_calls;
      state->last_place_local_order_id = request.local_order_id;
      state->last_quantity = request.quantity;
      state->last_side = request.side;
      state->last_reduce_only = request.reduce_only;
    }
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(const core::OrderCancelRequest&) noexcept {
    if (state != nullptr) {
      ++state->cancel_calls;
    }
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk};
  }

  SharedOrderSessionState* state{};
  bool running{false};
};

using Runtime = core::TradingRuntime<Strategy, BenchmarkOrderSession,
                                     market_data::RealtimeDataReader<>,
                                     core::TradingRuntimeDiagnostics>;

[[nodiscard]] config::StrategyConfig RuntimeConfig() {
  config::StrategyConfig config;
  config.name = "lead_lag";
  config.strategy_id = kStrategyId;
  config.order_capacity = kOrderCapacity;
  config.feedback.enabled = false;
  return config;
}

[[nodiscard]] Config BenchmarkConfig() {
  Config config;
  config.name = "lead_lag";
  config.version = "1.0";
  config.pairs.push_back(PairConfig{
      .symbol = "BTC_USDT",
      .symbol_id = kSymbolId,
      .lead_exchange = Exchange::kBinance,
      .lag_exchange = Exchange::kGate,
      .lag_taker_fee = 0.0,
      .max_lead_freshness_ms = benchmarking::kWideFreshnessGuardMs,
      .max_lag_freshness_ms = benchmarking::kWideFreshnessGuardMs,
      .trigger =
          TriggerConfig{
              .lead = 0.02,
              .close = 0.005,
              .lag_part = 0.5,
              .target_profit_rate = 0.0,
              .drift_period_ns = 1'000'000'000ULL,
              .drift_min_samples = 1,
              .drift_warmup_ns = 1,
              .quantile =
                  QuantileConfig{
                      .move = 0.75,
                      .up_min = 0.0,
                      .up_max = 0.10,
                      .down_min = -0.10,
                      .down_max = 0.0,
                      .precision = 0.000001,
                  },
          },
      .execute =
          ExecuteConfig{
              .open_notional = 1000.0,
              .trailing_stop = 0.05,
              .max_entry_spread = 0.02,
              .parallel = 1,
          },
      .bbo_record =
          BboRecordConfig{
              .window_ns = 1'000'000'000ULL,
              .stats_window_ns = 1'000'000'000ULL,
          },
      .lag_instrument =
          InstrumentMetadata{
              .symbol_id = kSymbolId,
              .exchange = Exchange::kGate,
              .exchange_symbol = "BTC_USDT",
              .price_tick = 0.1,
              .price_decimal_places = 1,
              .quantity_step = 1.0,
              .quantity_decimal_places = 0,
              .min_quantity = 1.0,
              .max_quantity = 20.0,
              .notional_multiplier = 1.0,
              .lag_taker_fee = 0.0,
          },
  });
  return config;
}

[[nodiscard]] BookTicker Ticker(Exchange exchange, std::int64_t local_ns,
                                double bid_price, double ask_price) noexcept {
  return BookTicker{
      .id = local_ns,
      .symbol_id = kSymbolId,
      .exchange = exchange,
      .exchange_ns = local_ns - 10,
      .local_ns = local_ns,
      .bid_price = bid_price,
      .bid_volume = 1.0,
      .ask_price = ask_price,
      .ask_volume = 1.0,
  };
}

[[nodiscard]] std::int64_t SeedBeforeOpenSignal(Runtime& runtime) noexcept {
  const auto base_ns = benchmarking::RealtimeNowNs();
  runtime.HandleBookTickerForTest(
      Ticker(Exchange::kGate, base_ns, 101.57, 102.02));
  runtime.HandleBookTickerForTest(
      Ticker(Exchange::kBinance, base_ns + 1'000, 100.0, 101.0));
  return base_ns + 2'000;
}

[[nodiscard]] BookTicker OpenLongTriggerTicker(std::int64_t event_ns) noexcept {
  return Ticker(Exchange::kBinance, event_ns, 112.0, 113.0);
}

void BM_LeadLagRuntimeOpenSignalSubmitPathLatency(benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    state.PauseTiming();
    SharedOrderSessionState session_state;
    auto runtime_result = Runtime::CreateForTest(
        RuntimeConfig(),
        [&session_state] { return BenchmarkOrderSession{&session_state}; },
        BenchmarkConfig());
    if (!runtime_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(runtime_result.error.c_str());
      return;
    }

    Runtime& runtime = *runtime_result.value;
    const std::int64_t trigger_event_ns = SeedBeforeOpenSignal(runtime);

    const BookTicker trigger_ticker = OpenLongTriggerTicker(trigger_event_ns);
    state.ResumeTiming();
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    runtime.HandleBookTickerForTest(trigger_ticker);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.PauseTiming();

    if (session_state.place_calls != 1 ||
        !std::isfinite(session_state.last_quantity) ||
        session_state.last_quantity <= 0.0 || session_state.last_reduce_only) {
      state.ResumeTiming();
      state.SkipWithError("lead lag submit path did not place one open order");
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    benchmark::DoNotOptimize(session_state.last_place_local_order_id);
    benchmark::DoNotOptimize(session_state.last_quantity);
    benchmark::DoNotOptimize(session_state.last_side);
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "orders", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_LeadLagRuntimeOpenSignalSubmitPathLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila::strategy::leadlag
