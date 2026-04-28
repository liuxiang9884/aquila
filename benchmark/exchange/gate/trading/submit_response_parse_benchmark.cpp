#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "exchange/gate/trading/submit_response_parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>
#include <simdjson.h>
#include <yyjson.h>

using namespace aquila::exchange::gate::trading;
using namespace aquila::websocket::benchmarking;

namespace {

constexpr size_t kBatchSize = 32;

constexpr std::string_view kOrderPlaceAckEcho = R"json({
  "request_id": "request-id-1",
  "ack": true,
  "header": {
    "response_time": "1681195484268",
    "status": "200",
    "channel": "futures.order_place",
    "event": "api",
    "client_id": "::1-0x140001a2600",
    "x_in_time": 1681985856667508,
    "x_out_time": 1681985856667598,
    "conn_trace_id": "1bde5aaa0acf2f5f48edfd4392e1fa68",
    "trace_id": "e410abb5f74b4afc519e67920548838d",
    "conn_id": "5e74253e9c793974",
    "x_gate_ratelimit_requests_remain": 99,
    "x_gate_ratelimit_limit": 100,
    "x_gate_ratelimit_reset_timestamp": 1681195484268
  },
  "data": {
    "result": {
      "req_id": "request-id-1",
      "req_header": null,
      "req_param": {
        "contract": "BTC_USDT",
        "size": "10",
        "price": "31503.280000",
        "tif": "gtc",
        "text": "t-my-custom-id"
      }
    }
  }
})json";

void BenchmarkOrderPlaceAckEchoYyjsonDefault(benchmark::State& state) {
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t parsed_messages = 0;
  std::uint64_t accumulator = 0;

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t i = 0; i < kBatchSize; ++i) {
      const auto parsed = ParseGateSubmitResponse(kOrderPlaceAckEcho);
      if (parsed.parse_status != GateSubmitParseStatus::kOk ||
          parsed.kind != GateSubmitResponseKind::kAck) {
        state.SkipWithError("parse failed");
        return;
      }
      accumulator ^= parsed.request_id_hash ^ parsed.req_id_hash;
      ++parsed_messages;
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_message_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_message_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_message_ns));
  }

  benchmark::DoNotOptimize(accumulator);
  state.counters["payload_bytes"] =
      static_cast<double>(kOrderPlaceAckEcho.size());
  SetLatencyCounters(state, std::move(samples_ns), "parsed_messages",
                     parsed_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "gate-order-place-ack-echo",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkOrderPlaceAckEchoYyjsonPool(benchmark::State& state) {
  const size_t pool_bytes =
      yyjson_read_max_memory_usage(kOrderPlaceAckEcho.size(), YYJSON_READ_NOFLAG);
  if (pool_bytes == 0) {
    state.SkipWithError("invalid yyjson pool size");
    return;
  }

  auto pool = std::make_unique<std::byte[]>(pool_bytes);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t parsed_messages = 0;
  std::uint64_t accumulator = 0;

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t i = 0; i < kBatchSize; ++i) {
      yyjson_alc allocator{};
      if (!yyjson_alc_pool_init(&allocator, pool.get(), pool_bytes)) {
        state.SkipWithError("yyjson pool init failed");
        return;
      }
      const auto parsed =
          ParseGateSubmitResponse(kOrderPlaceAckEcho, &allocator);
      if (parsed.parse_status != GateSubmitParseStatus::kOk ||
          parsed.kind != GateSubmitResponseKind::kAck) {
        state.SkipWithError("parse failed");
        return;
      }
      accumulator ^= parsed.request_id_hash ^ parsed.req_id_hash;
      ++parsed_messages;
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_message_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_message_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_message_ns));
  }

  benchmark::DoNotOptimize(accumulator);
  state.counters["payload_bytes"] =
      static_cast<double>(kOrderPlaceAckEcho.size());
  state.counters["pool_bytes"] = static_cast<double>(pool_bytes);
  SetLatencyCounters(state, std::move(samples_ns), "parsed_messages",
                     parsed_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "gate-order-place-ack-echo",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkOrderPlaceAckEchoYyjsonDefaultPaddedView(
    benchmark::State& state) {
  std::vector<char> scratch(kOrderPlaceAckEcho.size() +
                            simdjson::SIMDJSON_PADDING);
  std::copy(kOrderPlaceAckEcho.begin(), kOrderPlaceAckEcho.end(),
            scratch.begin());
  const std::string_view payload(scratch.data(), kOrderPlaceAckEcho.size());
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t parsed_messages = 0;
  std::uint64_t accumulator = 0;

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t i = 0; i < kBatchSize; ++i) {
      const auto parsed = ParseGateSubmitResponse(payload);
      if (parsed.parse_status != GateSubmitParseStatus::kOk ||
          parsed.kind != GateSubmitResponseKind::kAck) {
        state.SkipWithError("parse failed");
        return;
      }
      accumulator ^= parsed.request_id_hash ^ parsed.req_id_hash;
      ++parsed_messages;
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_message_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_message_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_message_ns));
  }

  benchmark::DoNotOptimize(accumulator);
  state.counters["payload_bytes"] =
      static_cast<double>(kOrderPlaceAckEcho.size());
  state.counters["scratch_bytes"] = static_cast<double>(scratch.size());
  SetLatencyCounters(state, std::move(samples_ns), "parsed_messages",
                     parsed_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "gate-order-place-ack-echo",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkOrderPlaceAckEchoYyjsonInsituCopy(benchmark::State& state) {
  std::vector<char> scratch(kOrderPlaceAckEcho.size() + YYJSON_PADDING_SIZE);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t parsed_messages = 0;
  std::uint64_t accumulator = 0;

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t i = 0; i < kBatchSize; ++i) {
      std::copy(kOrderPlaceAckEcho.begin(), kOrderPlaceAckEcho.end(),
                scratch.begin());
      const auto parsed =
          ParseGateSubmitResponseInsitu(scratch, kOrderPlaceAckEcho.size());
      if (parsed.parse_status != GateSubmitParseStatus::kOk ||
          parsed.kind != GateSubmitResponseKind::kAck) {
        state.SkipWithError("parse failed");
        return;
      }
      accumulator ^= parsed.request_id_hash ^ parsed.req_id_hash;
      ++parsed_messages;
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_message_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_message_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_message_ns));
  }

  benchmark::DoNotOptimize(accumulator);
  state.counters["payload_bytes"] =
      static_cast<double>(kOrderPlaceAckEcho.size());
  state.counters["scratch_bytes"] = static_cast<double>(scratch.size());
  SetLatencyCounters(state, std::move(samples_ns), "parsed_messages",
                     parsed_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "gate-order-place-ack-echo",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkOrderPlaceAckEchoYyjsonInsituCopyPool(benchmark::State& state) {
  const size_t pool_bytes = yyjson_read_max_memory_usage(
      kOrderPlaceAckEcho.size(), YYJSON_READ_INSITU);
  if (pool_bytes == 0) {
    state.SkipWithError("invalid yyjson pool size");
    return;
  }

  auto pool = std::make_unique<std::byte[]>(pool_bytes);
  std::vector<char> scratch(kOrderPlaceAckEcho.size() + YYJSON_PADDING_SIZE);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t parsed_messages = 0;
  std::uint64_t accumulator = 0;

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t i = 0; i < kBatchSize; ++i) {
      std::copy(kOrderPlaceAckEcho.begin(), kOrderPlaceAckEcho.end(),
                scratch.begin());
      yyjson_alc allocator{};
      if (!yyjson_alc_pool_init(&allocator, pool.get(), pool_bytes)) {
        state.SkipWithError("yyjson pool init failed");
        return;
      }
      const auto parsed = ParseGateSubmitResponseInsitu(
          scratch, kOrderPlaceAckEcho.size(), &allocator);
      if (parsed.parse_status != GateSubmitParseStatus::kOk ||
          parsed.kind != GateSubmitResponseKind::kAck) {
        state.SkipWithError("parse failed");
        return;
      }
      accumulator ^= parsed.request_id_hash ^ parsed.req_id_hash;
      ++parsed_messages;
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_message_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_message_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_message_ns));
  }

  benchmark::DoNotOptimize(accumulator);
  state.counters["payload_bytes"] =
      static_cast<double>(kOrderPlaceAckEcho.size());
  state.counters["pool_bytes"] = static_cast<double>(pool_bytes);
  state.counters["scratch_bytes"] = static_cast<double>(scratch.size());
  SetLatencyCounters(state, std::move(samples_ns), "parsed_messages",
                     parsed_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "gate-order-place-ack-echo",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkOrderPlaceAckEchoSimdjsonOnDemandCopy(benchmark::State& state) {
  simdjson::ondemand::parser parser;
  std::vector<char> scratch(kOrderPlaceAckEcho.size() +
                            simdjson::SIMDJSON_PADDING);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t parsed_messages = 0;
  std::uint64_t accumulator = 0;

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t i = 0; i < kBatchSize; ++i) {
      std::copy(kOrderPlaceAckEcho.begin(), kOrderPlaceAckEcho.end(),
                scratch.begin());
      const auto parsed = ParseGateSubmitResponseSimdjson(
          scratch, kOrderPlaceAckEcho.size(), parser);
      if (parsed.parse_status != GateSubmitParseStatus::kOk ||
          parsed.kind != GateSubmitResponseKind::kAck) {
        state.SkipWithError("parse failed");
        return;
      }
      accumulator ^= parsed.request_id_hash ^ parsed.req_id_hash;
      ++parsed_messages;
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_message_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_message_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_message_ns));
  }

  benchmark::DoNotOptimize(accumulator);
  state.counters["payload_bytes"] =
      static_cast<double>(kOrderPlaceAckEcho.size());
  state.counters["scratch_bytes"] = static_cast<double>(scratch.size());
  SetLatencyCounters(state, std::move(samples_ns), "parsed_messages",
                     parsed_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "gate-order-place-ack-echo",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

void BenchmarkOrderPlaceAckEchoSimdjsonOnDemandPaddedView(
    benchmark::State& state) {
  simdjson::ondemand::parser parser;
  std::vector<char> scratch(kOrderPlaceAckEcho.size() +
                            simdjson::SIMDJSON_PADDING);
  std::copy(kOrderPlaceAckEcho.begin(), kOrderPlaceAckEcho.end(),
            scratch.begin());
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t parsed_messages = 0;
  std::uint64_t accumulator = 0;

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t i = 0; i < kBatchSize; ++i) {
      const auto parsed = ParseGateSubmitResponseSimdjson(
          scratch, kOrderPlaceAckEcho.size(), parser);
      if (parsed.parse_status != GateSubmitParseStatus::kOk ||
          parsed.kind != GateSubmitResponseKind::kAck) {
        state.SkipWithError("parse failed");
        return;
      }
      accumulator ^= parsed.request_id_hash ^ parsed.req_id_hash;
      ++parsed_messages;
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    const auto per_message_ns =
        static_cast<double>(elapsed_ns) / static_cast<double>(kBatchSize);
    state.SetIterationTime(per_message_ns / 1'000'000'000.0);
    samples_ns.push_back(static_cast<std::uint64_t>(per_message_ns));
  }

  benchmark::DoNotOptimize(accumulator);
  state.counters["payload_bytes"] =
      static_cast<double>(kOrderPlaceAckEcho.size());
  state.counters["scratch_bytes"] = static_cast<double>(scratch.size());
  SetLatencyCounters(state, std::move(samples_ns), "parsed_messages",
                     parsed_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "gate-order-place-ack-echo",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

}  // namespace

BENCHMARK(BenchmarkOrderPlaceAckEchoYyjsonDefault)
    ->Name("gate_submit_response_parse_order_place_ack_echo_yyjson_default")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkOrderPlaceAckEchoYyjsonPool)
    ->Name("gate_submit_response_parse_order_place_ack_echo_yyjson_pool")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkOrderPlaceAckEchoYyjsonDefaultPaddedView)
    ->Name(
        "gate_submit_response_parse_order_place_ack_echo_yyjson_default_padded_view")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkOrderPlaceAckEchoYyjsonInsituCopy)
    ->Name("gate_submit_response_parse_order_place_ack_echo_yyjson_insitu_copy")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkOrderPlaceAckEchoYyjsonInsituCopyPool)
    ->Name(
        "gate_submit_response_parse_order_place_ack_echo_yyjson_insitu_copy_pool")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkOrderPlaceAckEchoSimdjsonOnDemandCopy)
    ->Name(
        "gate_submit_response_parse_order_place_ack_echo_simdjson_ondemand_copy")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkOrderPlaceAckEchoSimdjsonOnDemandPaddedView)
    ->Name(
        "gate_submit_response_parse_order_place_ack_echo_simdjson_ondemand_padded_view")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
