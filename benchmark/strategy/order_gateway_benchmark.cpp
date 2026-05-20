#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/common/types.h"
#include "core/trading/order_manager.h"
#include "core/trading/order_types.h"

namespace aquila::core {
namespace {

constexpr std::int64_t kLocalOrderId = 12345;
constexpr std::uint64_t kExchangeOrderId = 36028827892199865ULL;
constexpr std::string_view kContract = "BTC_USDT";
constexpr std::string_view kPriceText = "81000";
constexpr std::size_t kPlaceBatchSize = 8192;
constexpr std::size_t kPlaceLatencyIterations = 4096;

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  std::uint64_t place_calls{0};
  std::uint64_t cancel_calls{0};
  std::uint64_t last_place_local_order_id{0};
  std::uint64_t last_cancel_local_order_id{0};

  SendResult PlaceOrder(const StrategyOrder& order) noexcept {
    ++place_calls;
    last_place_local_order_id = order.local_order_id;
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(const StrategyOrder& order) noexcept {
    ++cancel_calls;
    last_cancel_local_order_id = order.local_order_id;
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk};
  }
};

[[nodiscard]] constexpr OrderCreateRequest MakeGateLimitRequest() noexcept {
  return OrderCreateRequest{.exchange = Exchange::kGate,
                            .symbol_id = 7,
                            .symbol = kContract,
                            .side = OrderSide::kBuy,
                            .time_in_force = TimeInForce::kGoodTillCancel,
                            .quantity = 1,
                            .price_text = kPriceText,
                            .reduce_only = false};
}

void SetOutlierCounters(benchmark::State& state,
                        std::span<const std::uint64_t> samples_ns) {
  std::uint64_t over_1us = 0;
  std::uint64_t over_2us = 0;
  std::uint64_t over_5us = 0;
  std::uint64_t max_index = 0;
  std::uint64_t max_ns = 0;

  for (std::size_t i = 0; i < samples_ns.size(); ++i) {
    const std::uint64_t sample_ns = samples_ns[i];
    over_1us += sample_ns > 1'000 ? 1 : 0;
    over_2us += sample_ns > 2'000 ? 1 : 0;
    over_5us += sample_ns > 5'000 ? 1 : 0;
    if (sample_ns > max_ns) {
      max_ns = sample_ns;
      max_index = static_cast<std::uint64_t>(i);
    }
  }

  state.counters["over_1us"] = static_cast<double>(over_1us);
  state.counters["over_2us"] = static_cast<double>(over_2us);
  state.counters["over_5us"] = static_cast<double>(over_5us);
  state.counters["max_index"] = static_cast<double>(max_index);
}

void BM_OrderManagerPlaceLimitOrder(benchmark::State& state) {
  FakeOrderSession session;
  std::optional<OrderManager<FakeOrderSession>> order_manager;
  std::size_t batch_count = kPlaceBatchSize;

  for (auto _ : state) {
    if (batch_count == kPlaceBatchSize) {
      state.PauseTiming();
      order_manager.emplace(session, kPlaceBatchSize);
      batch_count = 0;
      state.ResumeTiming();
    }

    OrderPlaceResult placed =
        order_manager->PlaceLimitOrder(MakeGateLimitRequest());
    if (placed.status != OrderPlaceStatus::kOk) {
      state.SkipWithError("order manager place limit order failed");
      return;
    }
    ++batch_count;

    benchmark::DoNotOptimize(placed.local_order_id);
    benchmark::DoNotOptimize(session.last_place_local_order_id);
  }

  benchmark::DoNotOptimize(session.place_calls);
}

void BM_OrderManagerPlaceLimitOrderLatency(benchmark::State& state) {
  FakeOrderSession session;
  OrderManager<FakeOrderSession> order_manager(session,
                                               kPlaceLatencyIterations + 1);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kPlaceLatencyIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const OrderPlaceResult placed =
        order_manager.PlaceLimitOrder(MakeGateLimitRequest());
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    if (placed.status != OrderPlaceStatus::kOk) {
      state.SkipWithError("order manager place limit order failed");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);

    std::uint64_t local_order_id = placed.local_order_id;
    benchmark::DoNotOptimize(local_order_id);
    benchmark::DoNotOptimize(session.last_place_local_order_id);
  }

  SetOutlierCounters(state, samples_ns);
  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "orders", state.iterations());
  benchmark::DoNotOptimize(session.place_calls);
}

void BM_OrderManagerCancelAcceptedOrder(benchmark::State& state) {
  FakeOrderSession session;

  for (auto _ : state) {
    state.PauseTiming();
    OrderManager<FakeOrderSession> order_manager(session, 1);
    const OrderPlaceResult placed =
        order_manager.PlaceLimitOrder(MakeGateLimitRequest());
    if (placed.status != OrderPlaceStatus::kOk) {
      state.SkipWithError("order manager place setup failed");
      return;
    }
    order_manager.OnOrderResponse(OrderResponseEvent{
        .kind = OrderResponseKind::kAccepted,
        .local_order_id = placed.local_order_id,
        .exchange_order_id = kExchangeOrderId,
    });
    state.ResumeTiming();

    OrderCancelResult cancelled =
        order_manager.CancelOrder(placed.local_order_id);
    if (cancelled.status != OrderCancelStatus::kOk) {
      state.SkipWithError("order manager cancel order failed");
      return;
    }

    benchmark::DoNotOptimize(cancelled.local_order_id);
    benchmark::DoNotOptimize(session.last_cancel_local_order_id);
  }

  benchmark::DoNotOptimize(session.cancel_calls);
}

BENCHMARK(BM_OrderManagerPlaceLimitOrder);
BENCHMARK(BM_OrderManagerPlaceLimitOrderLatency)
    ->Iterations(kPlaceLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderManagerCancelAcceptedOrder);

}  // namespace
}  // namespace aquila::core
