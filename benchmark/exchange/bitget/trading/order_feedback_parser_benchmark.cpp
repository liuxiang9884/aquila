#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "core/trading/order_feedback_event.h"
#include "core/trading/order_feedback_shm.h"
#include "exchange/bitget/trading/order_feedback_parser.h"
#include <simdjson.h>

namespace aquila::bitget {
namespace {

constexpr std::int64_t kLocalReceiveNs = 1'750'034'397'080'123'456LL;
constexpr std::size_t kPercentileSampleCount = 20'000;

constexpr std::string_view kAccepted =
    R"({"action":"snapshot","arg":{"instType":"UTA","topic":"order"},"data":[{"category":"usdt-futures","orderId":"9988","clientOid":"a-72057594037927978","qty":"1.5","holdMode":"one_way_mode","marginMode":"crossed","cumExecQty":"0","avgPrice":"0","orderStatus":"new","updatedTime":"1750034397076"}]})";
constexpr std::string_view kPartialFilled =
    R"({"action":"snapshot","arg":{"instType":"UTA","topic":"order"},"data":[{"category":"usdt-futures","orderId":"9988","clientOid":"a-72057594037927978","qty":"1.5","holdMode":"one_way_mode","marginMode":"crossed","cumExecQty":"0.4","avgPrice":"100.25","orderStatus":"partially_filled","updatedTime":"1750034397076"}]})";
constexpr std::string_view kTerminal =
    R"({"action":"snapshot","arg":{"instType":"UTA","topic":"order"},"data":[{"category":"usdt-futures","orderId":"9988","clientOid":"a-72057594037927978","qty":"1.5","holdMode":"one_way_mode","marginMode":"crossed","cumExecQty":"1.5","avgPrice":"100.25","orderStatus":"filled","updatedTime":"1750034397076"}]})";
constexpr std::string_view kForeign =
    R"({"action":"snapshot","arg":{"instType":"UTA","topic":"order"},"data":[{"clientOid":"manual-42"}]})";
constexpr std::string_view kMalformedAquila =
    R"({"action":"snapshot","arg":{"instType":"UTA","topic":"order"},"data":[{"clientOid":"a-42"}]})";
constexpr std::string_view kTypicalBatch =
    R"({"action":"snapshot","arg":{"instType":"UTA","topic":"order"},"data":[{"category":"usdt-futures","orderId":"9988","clientOid":"a-72057594037927978","qty":"1.5","holdMode":"one_way_mode","marginMode":"crossed","cumExecQty":"0","avgPrice":"0","orderStatus":"new","updatedTime":"1750034397076"},{"category":"usdt-futures","orderId":"9989","clientOid":"a-72057594037927979","qty":"1.5","holdMode":"one_way_mode","marginMode":"crossed","cumExecQty":"0.4","avgPrice":"100.25","orderStatus":"partially_filled","updatedTime":"1750034397077"},{"category":"usdt-futures","orderId":"9990","clientOid":"a-72057594037927980","qty":"1.5","holdMode":"one_way_mode","marginMode":"crossed","cumExecQty":"1.5","avgPrice":"100.5","orderStatus":"filled","updatedTime":"1750034397078"}]})";

template <typename Operation>
void RecordPercentiles(benchmark::State& state, Operation&& operation) {
  state.PauseTiming();
  std::vector<std::uint64_t> samples(kPercentileSampleCount);
  for (std::uint64_t& sample : samples) {
    const auto start = std::chrono::steady_clock::now();
    operation();
    const auto end = std::chrono::steady_clock::now();
    sample = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count());
  }
  std::sort(samples.begin(), samples.end());
  const auto percentile = [&samples](double ratio) {
    const std::size_t index = static_cast<std::size_t>(
        ratio * static_cast<double>(samples.size() - 1));
    return samples[index];
  };
  state.counters["sample_count"] = static_cast<double>(kPercentileSampleCount);
  state.counters["p50_ns"] = static_cast<double>(percentile(0.50));
  state.counters["p99_ns"] = static_cast<double>(percentile(0.99));
  state.counters["p99_9_ns"] = static_cast<double>(percentile(0.999));
  state.ResumeTiming();
}

void RunParserBenchmark(benchmark::State& state, std::string_view payload) {
  simdjson::padded_string padded(payload);
  const std::string_view view(padded.data(), padded.size());
  simdjson::ondemand::parser parser;
  OrderFeedbackEvent event{};
  const auto operation = [&] {
    OrderFeedbackParserStats stats{};
    const OrderFeedbackParseResult result = ParseBitgetOrderFeedbackMessage(
        view, simdjson::SIMDJSON_PADDING, kLocalReceiveNs, parser, stats,
        [&event](const OrderFeedbackEvent& parsed) noexcept {
          event = parsed;
          return true;
        });
    OrderFeedbackParseStatus status = result.status;
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(event.local_order_id);
  };

  for (auto _ : state) {
    operation();
  }
  RecordPercentiles(state, operation);
}

void BenchmarkAccepted(benchmark::State& state) {
  RunParserBenchmark(state, kAccepted);
}

void BenchmarkPartialFilled(benchmark::State& state) {
  RunParserBenchmark(state, kPartialFilled);
}

void BenchmarkTerminal(benchmark::State& state) {
  RunParserBenchmark(state, kTerminal);
}

void BenchmarkForeignOwnershipFastPath(benchmark::State& state) {
  RunParserBenchmark(state, kForeign);
}

void BenchmarkMalformedAquilaContinuityPath(benchmark::State& state) {
  RunParserBenchmark(state, kMalformedAquila);
}

void BenchmarkTypicalBatchPublish(benchmark::State& state) {
  RunParserBenchmark(state, kTypicalBatch);
}

void BenchmarkParserToShmPublisherAndDrain(benchmark::State& state) {
  simdjson::padded_string padded(kAccepted);
  const std::string_view view(padded.data(), padded.size());
  simdjson::ondemand::parser parser;
  auto channel = std::make_unique<OrderFeedbackShmChannel>();
  order_feedback_shm_detail::InitializeLaneHeaders(*channel);
  OrderFeedbackShmPublisher publisher(*channel);
  OrderFeedbackLane& lane = channel->lanes[1];
  const auto operation = [&] {
    OrderFeedbackParserStats stats{};
    const OrderFeedbackParseResult result = ParseBitgetOrderFeedbackMessage(
        view, simdjson::SIMDJSON_PADDING, kLocalReceiveNs, parser, stats,
        [&publisher](const OrderFeedbackEvent& event) noexcept {
          return publisher.Publish(event);
        });
    OrderFeedbackEvent event{};
    bool popped = lane.queue.TryPop(event);
    OrderFeedbackParseStatus status = result.status;
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(popped);
    benchmark::DoNotOptimize(event.local_order_id);
  };

  for (auto _ : state) {
    operation();
  }
  RecordPercentiles(state, operation);
}

BENCHMARK(BenchmarkAccepted);
BENCHMARK(BenchmarkPartialFilled);
BENCHMARK(BenchmarkTerminal);
BENCHMARK(BenchmarkForeignOwnershipFastPath);
BENCHMARK(BenchmarkMalformedAquilaContinuityPath);
BENCHMARK(BenchmarkTypicalBatchPublish);
BENCHMARK(BenchmarkParserToShmPublisherAndDrain);

}  // namespace
}  // namespace aquila::bitget
