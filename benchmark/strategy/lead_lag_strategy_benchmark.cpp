#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <vector>

#include <benchmark/benchmark.h>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "core/trading/order_types.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/recorders.h"
#include "strategy/lead_lag/strategy.h"
#include "strategy/lead_lag/window_stats.h"

namespace aquila::strategy::leadlag {
namespace {

constexpr std::size_t kTraceRecordLimit = 1'000'000;
constexpr std::size_t kWindowCapacity = 16'384;
constexpr std::uint64_t kWindowNs = 1'000'000'000ULL;
constexpr std::uint64_t kStatsWindowNs = 30'000'000'000ULL;
constexpr char kTracePathEnv[] = "AQUILA_LEAD_LAG_TRACE";
constexpr char kDefaultTracePath[] =
    "/home/liuxiang/tardis/merged_book_ticker/ORDI_USDT/20260415.bin";

struct NoopContext {
  [[nodiscard]] core::OrderPlaceResult PlaceOrder(
      core::OrderCreateRequest) noexcept {
    return {};
  }

  bool RetireFinishedOrder(std::uint64_t) noexcept {
    return false;
  }
};

[[nodiscard]] Config MakeOrdiConfig() {
  Config config;
  config.name = "lead_lag";
  config.version = "1.0";
  config.pairs.push_back(PairConfig{
      .symbol = "ORDI_USDT",
      .symbol_id = 3,
      .lead_exchange = Exchange::kBinance,
      .lag_exchange = Exchange::kGate,
      .lag_taker_fee = 0.00016,
      .trigger =
          TriggerConfig{
              .lead = 0.0025,
              .close = 0.0005,
              .lag_part = 0.5,
              .target_profit_rate = 0.0,
              .drift_limit = 0.02,
              .drift_period_ns = 60'000'000'000ULL,
              .drift_min_samples = 20,
              .drift_warmup_ns = 30'000'000'000ULL,
              .quantile =
                  QuantileConfig{
                      .move = 0.75,
                      .up_min = 0.0,
                      .up_max = 0.02,
                      .down_min = -0.02,
                      .down_max = 0.0,
                      .precision = 0.000001,
                  },
          },
      .execute =
          ExecuteConfig{
              .open_notional = 100.0,
              .trailing_stop = 0.01,
              .max_entry_spread = 0.01,
              .parallel = 1,
          },
      .bbo_record =
          BboRecordConfig{
              .window_ns = 1'000'000'000ULL,
              .stats_window_ns = 30'000'000'000ULL,
          },
      .capacity =
          CapacityConfig{
              .extrema_window_capacity = 16'384,
              .move_queue_capacity = 16'384,
              .noise_window_capacity = 16'384,
              .spread_window_capacity = 16'384,
          },
  });
  return config;
}

[[nodiscard]] StrategyOptions SyntheticReplayOptions() noexcept {
  return StrategyOptions{
      .position_accounting = PositionAccountingMode::kSyntheticSignals,
  };
}

[[nodiscard]] std::filesystem::path TracePath() {
  const char* env_path = std::getenv(kTracePathEnv);
  if (env_path != nullptr && env_path[0] != '\0') {
    return std::filesystem::path{env_path};
  }
  return std::filesystem::path{kDefaultTracePath};
}

[[nodiscard]] std::vector<BookTicker> LoadTrace(std::size_t limit) {
  const std::filesystem::path path = TracePath();
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }

  std::vector<BookTicker> trace;
  trace.reserve(limit);
  while (trace.size() < limit) {
    BookTicker ticker{};
    input.read(reinterpret_cast<char*>(&ticker),
               static_cast<std::streamsize>(sizeof(BookTicker)));
    if (input.gcount() != static_cast<std::streamsize>(sizeof(BookTicker))) {
      break;
    }
    trace.push_back(ticker);
  }
  return trace;
}

[[nodiscard]] const std::vector<BookTicker>& OrdiTrace() {
  static const std::vector<BookTicker> trace = LoadTrace(kTraceRecordLimit);
  return trace;
}

[[nodiscard]] BookTicker Ticker(std::int64_t id, Exchange exchange,
                                double bid_price, double ask_price) noexcept {
  return BookTicker{
      .id = id,
      .symbol_id = 3,
      .exchange = exchange,
      .exchange_ns = id,
      .local_ns = id,
      .bid_price = bid_price,
      .bid_volume = 1.0,
      .ask_price = ask_price,
      .ask_volume = 1.0,
  };
}

void WarmActive(Strategy* strategy, NoopContext* context) noexcept {
  std::int64_t event_ns = 1'000'000'000;
  for (int i = 0; i < 25; ++i) {
    const double step = static_cast<double>(i % 2) * 0.01;
    strategy->OnBookTicker(
        Ticker(event_ns, Exchange::kGate, 101.50 + step, 102.00 + step),
        *context);
    strategy->OnBookTicker(
        Ticker(event_ns + 10, Exchange::kBinance, 100.00 + step, 101.00 + step),
        *context);
    event_ns += 1'500'000'000;
  }
}

void BM_LeadLagStrategyOnBookTickerRealTrace(benchmark::State& state) {
  const std::vector<BookTicker>& trace = OrdiTrace();
  if (trace.empty()) {
    state.SkipWithError("failed to load ORDI BookTicker trace");
    return;
  }

  Strategy strategy{MakeOrdiConfig(), SyntheticReplayOptions()};
  NoopContext context;
  std::size_t index = 0;
  std::uint64_t signals = 0;

  for (auto _ : state) {
    if (index == trace.size()) {
      state.PauseTiming();
      strategy = Strategy{MakeOrdiConfig(), SyntheticReplayOptions()};
      index = 0;
      state.ResumeTiming();
    }
    strategy.OnBookTicker(trace[index], context);
    const SignalDecision& decision = strategy.last_signal_decision();
    signals += decision.triggered ? 1U : 0U;
    SignalAction action = decision.action;
    benchmark::DoNotOptimize(action);
    ++index;
  }

  benchmark::DoNotOptimize(signals);
  state.SetItemsProcessed(state.iterations());
  state.counters["signals"] = benchmark::Counter(
      static_cast<double>(signals), benchmark::Counter::kAvgIterations);
}

void BM_LeadLagStrategyActiveLeadTickNoSignal(benchmark::State& state) {
  Strategy strategy{MakeOrdiConfig(), SyntheticReplayOptions()};
  NoopContext context;
  WarmActive(&strategy, &context);
  std::int64_t event_ns = 31'000'001'000;
  std::uint64_t signals = 0;

  for (auto _ : state) {
    const bool toggle = ((event_ns / 1'000) & 1) != 0;
    strategy.OnBookTicker(
        Ticker(event_ns, Exchange::kBinance, toggle ? 100.10 : 100.11,
               toggle ? 101.10 : 101.11),
        context);
    const SignalDecision& decision = strategy.last_signal_decision();
    signals += decision.triggered ? 1U : 0U;
    SignalAction action = decision.action;
    benchmark::DoNotOptimize(action);
    event_ns += 1'000;
  }

  benchmark::DoNotOptimize(signals);
  state.SetItemsProcessed(state.iterations());
}

void BM_LeadLagStrategyActiveLagTickNoSignal(benchmark::State& state) {
  Strategy strategy{MakeOrdiConfig(), SyntheticReplayOptions()};
  NoopContext context;
  WarmActive(&strategy, &context);
  std::int64_t event_ns = 31'000'001'000;
  std::uint64_t signals = 0;

  for (auto _ : state) {
    const bool toggle = ((event_ns / 1'000) & 1) != 0;
    strategy.OnBookTicker(
        Ticker(event_ns, Exchange::kGate, toggle ? 101.50 : 101.51,
               toggle ? 102.00 : 102.01),
        context);
    const SignalDecision& decision = strategy.last_signal_decision();
    signals += decision.triggered ? 1U : 0U;
    SignalAction action = decision.action;
    benchmark::DoNotOptimize(action);
    event_ns += 1'000;
  }

  benchmark::DoNotOptimize(signals);
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_LeadLagStrategyOnBookTickerRealTrace)
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagStrategyActiveLeadTickNoSignal)
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagStrategyActiveLagTickNoSignal)
    ->Unit(benchmark::kNanosecond);

void BM_MeanStdWindowUpdateOnly(benchmark::State& state) {
  MeanStdWindow window;
  window.Init(kWindowNs, kWindowCapacity);
  std::int64_t event_ns = 1'000'000'000;
  double value = 100.0;

  for (auto _ : state) {
    window.Update(event_ns, value);
    benchmark::DoNotOptimize(window.size());
    event_ns += 1'000;
    value += 0.0001;
  }

  state.SetItemsProcessed(state.iterations());
}

void BM_MeanStdWindowStddevOnly(benchmark::State& state) {
  MeanStdWindow window;
  window.Init(kWindowNs, kWindowCapacity);
  std::int64_t event_ns = 1'000'000'000;
  for (std::size_t i = 0; i < kWindowCapacity; ++i) {
    window.Update(event_ns, 100.0 + static_cast<double>(i % 100) * 0.0001);
    event_ns += 1'000;
  }

  double value = 0.0;
  for (auto _ : state) {
    value += window.stddev();
    benchmark::DoNotOptimize(value);
  }

  state.SetItemsProcessed(state.iterations());
}

void BM_MeanStdWindowUpdateAndStddev(benchmark::State& state) {
  MeanStdWindow window;
  window.Init(kWindowNs, kWindowCapacity);
  std::int64_t event_ns = 1'000'000'000;
  double value = 100.0;
  double stddev = 0.0;

  for (auto _ : state) {
    window.Update(event_ns, value);
    stddev += window.stddev();
    benchmark::DoNotOptimize(stddev);
    event_ns += 1'000;
    value += 0.0001;
  }

  state.SetItemsProcessed(state.iterations());
}

void BM_NoiseStateUpdate(benchmark::State& state) {
  NoiseState noise;
  noise.Init(kWindowNs, kStatsWindowNs, kWindowCapacity);
  std::int64_t event_ns = 1'000'000'000;
  double bid = 100.0;
  double noise_value = 0.0;

  for (auto _ : state) {
    noise.Update(QuoteSnapshot{
        .event_ns = event_ns,
        .bid_price = bid,
        .ask_price = bid + 0.001,
    });
    noise_value += noise.value();
    benchmark::DoNotOptimize(noise_value);
    event_ns += 1'000;
    bid += 0.0001;
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_MeanStdWindowUpdateOnly)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_MeanStdWindowStddevOnly)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_MeanStdWindowUpdateAndStddev)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_NoiseStateUpdate)->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila::strategy::leadlag
