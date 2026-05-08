#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

#include "core/trading/order_pool.h"

namespace aquila {
namespace {

constexpr std::size_t kCapacity = 8192;

struct BenchOrder {
  std::uint64_t local_order_id{0};
  std::int64_t payload{0};
};

void BM_OrderPoolCreateUntilCapacity(benchmark::State& state) {
  std::int64_t created_total = 0;

  for (auto _ : state) {
    state.PauseTiming();
    OrderPool<BenchOrder> pool(kCapacity);
    state.ResumeTiming();

    for (std::size_t i = 0; i < kCapacity; ++i) {
      BenchOrder* order = pool.Create();
      if (order == nullptr) {
        state.SkipWithError("order pool create failed before capacity");
        return;
      }
      order->payload = static_cast<std::int64_t>(i);
      benchmark::DoNotOptimize(order);
      benchmark::DoNotOptimize(order->local_order_id);
      ++created_total;
    }
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(created_total);
}

void BM_OrderPoolFindLiveOrders(benchmark::State& state) {
  OrderPool<BenchOrder> pool(kCapacity);
  std::vector<std::uint64_t> local_order_ids;
  local_order_ids.reserve(kCapacity);

  for (std::size_t i = 0; i < kCapacity; ++i) {
    BenchOrder* order = pool.Create();
    if (order == nullptr) {
      state.SkipWithError("order pool setup failed");
      return;
    }
    order->payload = static_cast<std::int64_t>(i);
    local_order_ids.push_back(order->local_order_id);
  }

  std::size_t index = 0;
  for (auto _ : state) {
    BenchOrder* found = pool.Find(local_order_ids[index]);
    if (found == nullptr) {
      state.SkipWithError("live order lookup failed");
      return;
    }
    benchmark::DoNotOptimize(found);
    benchmark::DoNotOptimize(found->payload);

    ++index;
    if (index == local_order_ids.size()) {
      index = 0;
    }
  }
}

void BM_OrderPoolCreateFindEraseRecycleLoop(benchmark::State& state) {
  OrderPool<BenchOrder> pool(kCapacity);

  for (auto _ : state) {
    BenchOrder* order = pool.Create();
    if (order == nullptr) {
      state.SkipWithError("order pool create failed");
      return;
    }
    std::uint64_t local_order_id = order->local_order_id;
    order->payload = local_order_id;

    BenchOrder* found = pool.Find(local_order_id);
    if (found == nullptr) {
      state.SkipWithError("created order lookup failed");
      return;
    }

    benchmark::DoNotOptimize(order);
    benchmark::DoNotOptimize(found);
    benchmark::DoNotOptimize(local_order_id);
    benchmark::ClobberMemory();

    if (!pool.Erase(local_order_id)) {
      state.SkipWithError("order erase failed");
      return;
    }
  }

  benchmark::DoNotOptimize(pool.size());
}

BENCHMARK(BM_OrderPoolCreateUntilCapacity)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderPoolFindLiveOrders)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderPoolCreateFindEraseRecycleLoop)->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila
