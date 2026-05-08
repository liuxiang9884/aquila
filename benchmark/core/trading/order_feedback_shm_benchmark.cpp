#include <cstddef>
#include <cstdint>
#include <memory>

#include <benchmark/benchmark.h>

#include "core/trading/order_feedback_shm.h"
#include "core/trading/order_id.h"

namespace aquila {
namespace {

constexpr std::uint8_t kBenchmarkStrategyId = 3;
constexpr std::uint64_t kReaderRunId = 0xF00D'0001ULL;

std::unique_ptr<OrderFeedbackShmChannel> MakeChannelForBenchmark() {
  auto channel = std::make_unique<OrderFeedbackShmChannel>();
  order_feedback_shm_detail::InitializeLaneHeaders(*channel);
  return channel;
}

OrderFeedbackEvent MakeAcceptedEvent(std::uint8_t strategy_id,
                                     std::uint64_t strategy_order_id) {
  OrderFeedbackEvent event{};
  event.kind = OrderFeedbackKind::kAccepted;
  event.local_order_id =
      LocalOrderIdCodec::Encode(strategy_id, strategy_order_id);
  event.exchange_order_id = 9000 + strategy_order_id;
  event.cumulative_filled_quantity = 10;
  event.left_quantity = 90;
  event.cancelled_quantity = 0;
  event.fill_price = 1234.5;
  event.role = OrderRole::kMaker;
  event.exchange_update_ns = 111;
  event.local_receive_ns = 222;
  return event;
}

void BM_OrderFeedbackShmPublishThenDrain(benchmark::State& state) {
  auto channel = MakeChannelForBenchmark();
  OrderFeedbackShmPublisher publisher(*channel);
  OrderFeedbackEvent event = MakeAcceptedEvent(kBenchmarkStrategyId, 1);
  OrderFeedbackEvent popped{};
  std::uint64_t strategy_order_id = 1;

  for (auto _ : state) {
    event.local_order_id =
        LocalOrderIdCodec::Encode(kBenchmarkStrategyId, strategy_order_id);
    bool published = publisher.Publish(event);
    benchmark::DoNotOptimize(published);
    if (!published) {
      state.SkipWithError("order feedback publish failed");
      return;
    }

    bool popped_event =
        channel->lanes[kBenchmarkStrategyId].queue.TryPop(popped);
    benchmark::DoNotOptimize(popped_event);
    benchmark::DoNotOptimize(popped);
    if (!popped_event) {
      state.SkipWithError("order feedback publish drain failed");
      return;
    }

    ++strategy_order_id;
  }

  benchmark::DoNotOptimize(publisher.published_count());
  benchmark::ClobberMemory();
  state.SetItemsProcessed(state.iterations());
}

void BM_OrderFeedbackShmPollOneWithRefill(benchmark::State& state) {
  auto channel = MakeChannelForBenchmark();
  OrderFeedbackEvent event = MakeAcceptedEvent(kBenchmarkStrategyId, 1);
  if (!channel->lanes[kBenchmarkStrategyId].queue.TryPush(event)) {
    state.SkipWithError("order feedback poll prefill failed");
    return;
  }

  auto reader_result = OrderFeedbackShmReader::Claim(
      *channel, kBenchmarkStrategyId, kReaderRunId);
  if (!reader_result.ok) {
    state.SkipWithError(reader_result.error.c_str());
    return;
  }

  std::uint64_t observed_local_order_id = 0;
  std::uint64_t strategy_order_id = 2;

  for (auto _ : state) {
    std::size_t polled = reader_result.value.Poll(
        1, [&observed_local_order_id](const OrderFeedbackEvent& polled_event) {
          observed_local_order_id = polled_event.local_order_id;
        });
    benchmark::DoNotOptimize(polled);
    benchmark::DoNotOptimize(observed_local_order_id);
    if (polled != 1) {
      state.SkipWithError("order feedback poll did not consume one event");
      return;
    }

    event.local_order_id =
        LocalOrderIdCodec::Encode(kBenchmarkStrategyId, strategy_order_id);
    bool refilled = channel->lanes[kBenchmarkStrategyId].queue.TryPush(event);
    benchmark::DoNotOptimize(refilled);
    if (!refilled) {
      state.SkipWithError("order feedback poll refill failed");
      return;
    }

    ++strategy_order_id;
  }

  benchmark::DoNotOptimize(reader_result.value.consumed_count());
  benchmark::ClobberMemory();
  state.SetItemsProcessed(state.iterations());
}

void BM_OrderFeedbackShmPublishPollLoop(benchmark::State& state) {
  auto channel = MakeChannelForBenchmark();
  OrderFeedbackShmPublisher publisher(*channel);
  auto reader_result = OrderFeedbackShmReader::Claim(
      *channel, kBenchmarkStrategyId, kReaderRunId);
  if (!reader_result.ok) {
    state.SkipWithError(reader_result.error.c_str());
    return;
  }

  OrderFeedbackEvent event = MakeAcceptedEvent(kBenchmarkStrategyId, 1);
  std::uint64_t observed_local_order_id = 0;
  std::uint64_t strategy_order_id = 1;

  for (auto _ : state) {
    event.local_order_id =
        LocalOrderIdCodec::Encode(kBenchmarkStrategyId, strategy_order_id);
    bool published = publisher.Publish(event);
    benchmark::DoNotOptimize(published);
    if (!published) {
      state.SkipWithError("order feedback publish-poll publish failed");
      return;
    }

    std::size_t polled = reader_result.value.Poll(
        1, [&observed_local_order_id](const OrderFeedbackEvent& polled_event) {
          observed_local_order_id = polled_event.local_order_id;
        });
    benchmark::DoNotOptimize(polled);
    benchmark::DoNotOptimize(observed_local_order_id);
    if (polled != 1) {
      state.SkipWithError("order feedback publish-poll poll failed");
      return;
    }

    ++strategy_order_id;
  }

  benchmark::DoNotOptimize(publisher.published_count());
  benchmark::DoNotOptimize(reader_result.value.consumed_count());
  benchmark::ClobberMemory();
  state.SetItemsProcessed(state.iterations());
}

void BM_OrderFeedbackShmPublishGlobalGapThenDrain(benchmark::State& state) {
  auto channel = MakeChannelForBenchmark();
  OrderFeedbackShmPublisher publisher(*channel);
  OrderFeedbackEvent popped{};

  for (auto _ : state) {
    bool published = publisher.PublishGlobalGap(
        OrderFeedbackGapReason::kSessionDisconnected, 123);
    benchmark::DoNotOptimize(published);
    if (!published) {
      state.SkipWithError("order feedback global gap publish failed");
      return;
    }

    for (std::uint32_t i = 0; i < kMaxOrderFeedbackStrategies; ++i) {
      bool popped_event = channel->lanes[i].queue.TryPop(popped);
      benchmark::DoNotOptimize(popped_event);
      benchmark::DoNotOptimize(popped);
      if (!popped_event) {
        state.SkipWithError("order feedback global gap drain failed");
        return;
      }
    }
  }

  benchmark::DoNotOptimize(publisher.published_count());
  benchmark::ClobberMemory();
  state.SetItemsProcessed(state.iterations() * kMaxOrderFeedbackStrategies);
}

// These cases keep queues non-full or non-empty inside the timed loop, so
// single-operation cases include drain/refill maintenance rather than pure
// isolated operation latency.
BENCHMARK(BM_OrderFeedbackShmPublishThenDrain)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderFeedbackShmPollOneWithRefill)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderFeedbackShmPublishPollLoop)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderFeedbackShmPublishGlobalGapThenDrain)
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila
