#include <cstdint>
#include <optional>
#include <string_view>

#include <benchmark/benchmark.h>

#include "core/common/types.h"
#include "strategy/order_types.h"
#include "strategy/strategy.h"

namespace aquila::strategy {
namespace {

constexpr std::int64_t kLocalOrderId = 12345;
constexpr std::uint64_t kExchangeOrderId = 36028827892199865ULL;
constexpr std::string_view kContract = "BTC_USDT";
constexpr std::string_view kPriceText = "81000";
constexpr std::size_t kPlaceBatchSize = 8192;

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  std::uint64_t place_calls{0};
  std::uint64_t cancel_calls{0};
  std::int64_t last_place_local_order_id{0};
  std::int64_t last_cancel_local_order_id{0};

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

void BM_StrategyPlaceLimitOrder(benchmark::State& state) {
  FakeOrderSession session;
  std::optional<Strategy<FakeOrderSession>> strategy;
  std::size_t batch_count = kPlaceBatchSize;

  for (auto _ : state) {
    if (batch_count == kPlaceBatchSize) {
      state.PauseTiming();
      strategy.emplace(session, kPlaceBatchSize);
      batch_count = 0;
      state.ResumeTiming();
    }

    OrderPlaceResult placed = strategy->PlaceLimitOrder(MakeGateLimitRequest());
    if (placed.status != OrderPlaceStatus::kOk) {
      state.SkipWithError("strategy place limit order failed");
      return;
    }
    ++batch_count;

    benchmark::DoNotOptimize(placed.local_order_id);
    benchmark::DoNotOptimize(session.last_place_local_order_id);
  }

  benchmark::DoNotOptimize(session.place_calls);
}

void BM_StrategyCancelAcceptedOrder(benchmark::State& state) {
  FakeOrderSession session;

  for (auto _ : state) {
    state.PauseTiming();
    Strategy<FakeOrderSession> strategy(session, 1);
    const OrderPlaceResult placed =
        strategy.PlaceLimitOrder(MakeGateLimitRequest());
    if (placed.status != OrderPlaceStatus::kOk) {
      state.SkipWithError("strategy place setup failed");
      return;
    }
    strategy.OnOrderResponse(OrderResponseEvent{
        .kind = OrderResponseKind::kAccepted,
        .local_order_id = placed.local_order_id,
        .exchange_order_id = kExchangeOrderId,
    });
    state.ResumeTiming();

    OrderCancelResult cancelled = strategy.CancelOrder(placed.local_order_id);
    if (cancelled.status != OrderCancelStatus::kOk) {
      state.SkipWithError("strategy cancel order failed");
      return;
    }

    benchmark::DoNotOptimize(cancelled.local_order_id);
    benchmark::DoNotOptimize(session.last_cancel_local_order_id);
  }

  benchmark::DoNotOptimize(session.cancel_calls);
}

BENCHMARK(BM_StrategyPlaceLimitOrder);
BENCHMARK(BM_StrategyCancelAcceptedOrder);

}  // namespace
}  // namespace aquila::strategy
