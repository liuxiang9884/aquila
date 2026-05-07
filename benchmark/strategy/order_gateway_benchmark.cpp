#include <cstdint>
#include <string_view>

#include <benchmark/benchmark.h>

#include "core/common/types.h"
#include "exchange/gate/trading/order_types.h"
#include "exchange/gate/trading/gate_order_gateway.h"
#include "strategy/order_types.h"

namespace aquila::strategy {
namespace {

constexpr std::int64_t kLocalOrderId = 12345;
constexpr std::uint64_t kExchangeOrderId = 36028827892199865ULL;
constexpr std::string_view kContract = "BTC_USDT";
constexpr std::string_view kPriceText = "81000";

struct FakeGateOrderSession {
  gate::PlaceOrderRequest last_place_request{};
  gate::CancelOrderRequest last_cancel_request{};
  std::uint64_t place_calls{0};
  std::uint64_t cancel_calls{0};

  gate::OrderSendResult PlaceOrder(
      const gate::PlaceOrderRequest& request) noexcept {
    last_place_request = request;
    benchmark::ClobberMemory();
    return {.status = gate::OrderSendStatus::kOk,
            .request_sequence = ++place_calls};
  }

  gate::OrderSendResult CancelOrder(
      const gate::CancelOrderRequest& request) noexcept {
    last_cancel_request = request;
    benchmark::ClobberMemory();
    return {.status = gate::OrderSendStatus::kOk,
            .request_sequence = ++cancel_calls};
  }
};

[[nodiscard]] constexpr OrderDraft MakeGateLimitDraft() noexcept {
  return OrderDraft{.exchange = Exchange::kGate,
                    .symbol_id = 7,
                    .symbol = kContract,
                    .side = OrderSide::kBuy,
                    .type = OrderType::kLimit,
                    .time_in_force = TimeInForce::kGoodTillCancel,
                    .signed_quantity = 1,
                    .price_text = kPriceText,
                    .reduce_only = false};
}

[[nodiscard]] bool PrepareCachedOrder(
    GateOrderGateway<FakeGateOrderSession>& gateway,
    GateStrategyOrder& order) noexcept {
  order.local_order_id = kLocalOrderId;
  return gateway.PrepareOrder(order, MakeGateLimitDraft());
}

void BM_GateStrategyPrepareLimitOrder(benchmark::State& state) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);

  for (auto _ : state) {
    GateStrategyOrder order;
    order.local_order_id = kLocalOrderId;
    const bool prepared = gateway.PrepareOrder(order, MakeGateLimitDraft());
    if (!prepared) {
      state.SkipWithError("Gate strategy order prepare failed");
      return;
    }

    benchmark::DoNotOptimize(order.gate.wire.local_order_id);
    benchmark::DoNotOptimize(order.gate.wire.contract.data());
    benchmark::DoNotOptimize(order.gate.wire.contract.size());
    benchmark::DoNotOptimize(order.gate.wire.signed_size);
    benchmark::DoNotOptimize(order.gate.wire.price_text.data());
    benchmark::DoNotOptimize(order.gate.wire.price_text.size());
    benchmark::DoNotOptimize(order.gate.wire.text.data());
    benchmark::DoNotOptimize(order.gate.wire.text.size());
  }
}

void BM_GateStrategyPlaceCachedOrder(benchmark::State& state) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  if (!PrepareCachedOrder(gateway, order)) {
    state.SkipWithError("Gate strategy order prepare failed");
    return;
  }

  for (auto _ : state) {
    GatewaySendResult sent = gateway.PlaceOrder(order);
    if (sent.status != GatewaySendStatus::kOk) {
      state.SkipWithError("Gate strategy order place failed");
      return;
    }

    benchmark::DoNotOptimize(sent.status);
    benchmark::DoNotOptimize(session.last_place_request.wire.local_order_id);
    benchmark::DoNotOptimize(session.last_place_request.wire.contract.data());
    benchmark::DoNotOptimize(session.last_place_request.wire.contract.size());
    benchmark::DoNotOptimize(session.last_place_request.wire.signed_size);
  }

  benchmark::DoNotOptimize(session.place_calls);
  benchmark::DoNotOptimize(session.last_place_request.wire.local_order_id);
}

void BM_GateStrategyCancelCachedOrder(benchmark::State& state) {
  FakeGateOrderSession session;
  GateOrderGateway<FakeGateOrderSession> gateway(session);
  GateStrategyOrder order;
  if (!PrepareCachedOrder(gateway, order)) {
    state.SkipWithError("Gate strategy order prepare failed");
    return;
  }
  order.exchange_order_id = kExchangeOrderId;

  for (auto _ : state) {
    GatewaySendResult sent = gateway.CancelOrder(order);
    if (sent.status != GatewaySendStatus::kOk) {
      state.SkipWithError("Gate strategy order cancel failed");
      return;
    }

    benchmark::DoNotOptimize(sent.status);
    benchmark::DoNotOptimize(session.last_cancel_request.local_order_id);
    benchmark::DoNotOptimize(session.last_cancel_request.exchange_order_id);
  }

  benchmark::DoNotOptimize(session.cancel_calls);
  benchmark::DoNotOptimize(session.last_cancel_request.local_order_id);
  benchmark::DoNotOptimize(session.last_cancel_request.exchange_order_id);
}

BENCHMARK(BM_GateStrategyPrepareLimitOrder);
BENCHMARK(BM_GateStrategyPlaceCachedOrder);
BENCHMARK(BM_GateStrategyCancelCachedOrder);

}  // namespace
}  // namespace aquila::strategy
