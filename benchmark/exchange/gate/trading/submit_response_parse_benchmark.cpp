#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "exchange/gate/trading/submit_response_parser.h"
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

class YyjsonDoc {
 public:
  explicit YyjsonDoc(yyjson_doc* doc) noexcept : doc_(doc) {}
  YyjsonDoc(const YyjsonDoc&) = delete;
  YyjsonDoc& operator=(const YyjsonDoc&) = delete;
  ~YyjsonDoc() {
    if (doc_ != nullptr) {
      yyjson_doc_free(doc_);
    }
  }

  [[nodiscard]] yyjson_doc* get() const noexcept {
    return doc_;
  }

 private:
  yyjson_doc* doc_{nullptr};
};

std::string_view ReadYyjsonStringView(yyjson_val* value) noexcept {
  if (!yyjson_is_str(value)) {
    return {};
  }
  const char* text = yyjson_get_str(value);
  if (text == nullptr) {
    return {};
  }
  return std::string_view(text, yyjson_get_len(value));
}

bool ReadYyjsonBool(yyjson_val* value, bool* output) noexcept {
  if (!yyjson_is_bool(value)) {
    return false;
  }
  *output = yyjson_get_bool(value);
  return true;
}

bool ParseYyjsonUintString(std::string_view value,
                           std::uint64_t* output) noexcept {
  if (value.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  *output = parsed;
  return true;
}

bool ReadYyjsonUint64(yyjson_val* value, std::uint64_t* output) noexcept {
  if (yyjson_is_uint(value)) {
    *output = yyjson_get_uint(value);
    return true;
  }
  if (yyjson_is_int(value)) {
    const std::int64_t signed_value = yyjson_get_sint(value);
    if (signed_value < 0) {
      return false;
    }
    *output = static_cast<std::uint64_t>(signed_value);
    return true;
  }
  return ParseYyjsonUintString(ReadYyjsonStringView(value), output);
}

std::uint16_t ReadYyjsonStatusCode(yyjson_val* value) noexcept {
  std::uint64_t parsed = 0;
  if (!ReadYyjsonUint64(value, &parsed) || parsed > 999) {
    return 0;
  }
  return static_cast<std::uint16_t>(parsed);
}

std::uint64_t HashYyjsonStringValue(yyjson_val* value) noexcept {
  const std::string_view text = ReadYyjsonStringView(value);
  return text.empty() ? 0 : HashGateSubmitString(text);
}

GateSubmitResponse ParseYyjsonDocument(yyjson_doc* doc) noexcept {
  GateSubmitResponse response{};
  if (doc == nullptr) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  yyjson_val* root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    response.parse_status = GateSubmitParseStatus::kUnexpectedShape;
    return response;
  }

  response.parse_status = GateSubmitParseStatus::kOk;
  response.request_id_hash =
      HashYyjsonStringValue(yyjson_obj_get(root, "request_id"));

  bool ack = false;
  response.has_ack = ReadYyjsonBool(yyjson_obj_get(root, "ack"), &ack);
  response.ack = response.has_ack && ack;

  yyjson_val* header = yyjson_obj_get(root, "header");
  if (yyjson_is_obj(header)) {
    response.http_status =
        ReadYyjsonStatusCode(yyjson_obj_get(header, "status"));
    response.channel_is_order_place =
        ReadYyjsonStringView(yyjson_obj_get(header, "channel")) ==
        std::string_view("futures.order_place");
  }

  yyjson_val* data = yyjson_obj_get(root, "data");
  if (!yyjson_is_obj(data)) {
    return response;
  }

  yyjson_val* errs = yyjson_obj_get(data, "errs");
  if (yyjson_is_obj(errs)) {
    response.kind = GateSubmitResponseKind::kError;
    response.error_label_hash =
        HashYyjsonStringValue(yyjson_obj_get(errs, "label"));
    return response;
  }

  yyjson_val* result = yyjson_obj_get(data, "result");
  if (!yyjson_is_obj(result)) {
    return response;
  }

  if (response.ack) {
    response.kind = GateSubmitResponseKind::kAck;
    response.req_id_hash =
        HashYyjsonStringValue(yyjson_obj_get(result, "req_id"));
    return response;
  }

  response.kind = GateSubmitResponseKind::kResult;
  (void)ReadYyjsonUint64(yyjson_obj_get(result, "id"),
                         &response.exchange_order_id);
  response.text_hash = HashYyjsonStringValue(yyjson_obj_get(result, "text"));
  return response;
}

GateSubmitResponse ParseYyjsonAckMinimalDocument(yyjson_doc* doc) noexcept {
  GateSubmitResponse response{};
  if (doc == nullptr) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  yyjson_val* root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    response.parse_status = GateSubmitParseStatus::kUnexpectedShape;
    return response;
  }

  response.parse_status = GateSubmitParseStatus::kOk;
  response.request_id_hash =
      HashYyjsonStringValue(yyjson_obj_get(root, "request_id"));

  bool ack = false;
  response.has_ack = ReadYyjsonBool(yyjson_obj_get(root, "ack"), &ack);
  response.ack = response.has_ack && ack;
  if (response.ack) {
    response.kind = GateSubmitResponseKind::kAck;
  }
  return response;
}

GateSubmitResponse ParseGateSubmitResponseYyjson(
    std::string_view payload, const yyjson_alc* allocator = nullptr) noexcept {
  GateSubmitResponse response{};
  if (payload.empty()) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  yyjson_read_err error{};
  YyjsonDoc doc(yyjson_read_opts(const_cast<char*>(payload.data()),
                                 payload.size(), YYJSON_READ_NOFLAG, allocator,
                                 &error));
  if (doc.get() == nullptr) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  return ParseYyjsonDocument(doc.get());
}

GateSubmitResponse ParseGateSubmitAckMinimalYyjson(
    std::string_view payload, const yyjson_alc* allocator = nullptr) noexcept {
  GateSubmitResponse response{};
  if (payload.empty()) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  yyjson_read_err error{};
  YyjsonDoc doc(yyjson_read_opts(const_cast<char*>(payload.data()),
                                 payload.size(), YYJSON_READ_NOFLAG, allocator,
                                 &error));
  if (doc.get() == nullptr) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  return ParseYyjsonAckMinimalDocument(doc.get());
}

GateSubmitResponse ParseGateSubmitResponseYyjsonInsitu(
    std::span<char> padded_payload, size_t payload_size,
    const yyjson_alc* allocator = nullptr) noexcept {
  GateSubmitResponse response{};
  if (payload_size == 0 || payload_size > padded_payload.size() ||
      padded_payload.size() - payload_size < YYJSON_PADDING_SIZE) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  yyjson_read_err error{};
  YyjsonDoc doc(yyjson_read_opts(padded_payload.data(), payload_size,
                                 YYJSON_READ_INSITU, allocator, &error));
  if (doc.get() == nullptr) {
    response.parse_status = GateSubmitParseStatus::kInvalidJson;
    return response;
  }

  return ParseYyjsonDocument(doc.get());
}

void BenchmarkOrderPlaceAckEchoYyjsonDefault(benchmark::State& state) {
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t parsed_messages = 0;
  std::uint64_t accumulator = 0;

  for (auto _ : state) {
    const std::uint64_t start_ns = NowNs();
    for (size_t i = 0; i < kBatchSize; ++i) {
      const auto parsed = ParseGateSubmitResponseYyjson(kOrderPlaceAckEcho);
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
  const size_t pool_bytes = yyjson_read_max_memory_usage(
      kOrderPlaceAckEcho.size(), YYJSON_READ_NOFLAG);
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
          ParseGateSubmitResponseYyjson(kOrderPlaceAckEcho, &allocator);
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
      const auto parsed = ParseGateSubmitResponseYyjson(payload);
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

void BenchmarkOrderPlaceAckEchoYyjsonAckMinimalPaddedView(
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
      const auto parsed = ParseGateSubmitAckMinimalYyjson(payload);
      if (parsed.parse_status != GateSubmitParseStatus::kOk ||
          parsed.kind != GateSubmitResponseKind::kAck) {
        state.SkipWithError("parse failed");
        return;
      }
      const auto ack_bit =
          static_cast<std::uint64_t>(parsed.has_ack && parsed.ack);
      accumulator ^= parsed.request_id_hash ^ ack_bit;
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
      const auto parsed = ParseGateSubmitResponseYyjsonInsitu(
          scratch, kOrderPlaceAckEcho.size());
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
      const auto parsed = ParseGateSubmitResponseYyjsonInsitu(
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

void BenchmarkOrderPlaceAckEchoSimdjsonAckMinimalPaddedView(
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
      const auto parsed = ParseGateSubmitAckMinimalSimdjson(
          scratch, kOrderPlaceAckEcho.size(), parser);
      if (parsed.parse_status != GateSubmitParseStatus::kOk ||
          parsed.kind != GateSubmitResponseKind::kAck) {
        state.SkipWithError("parse failed");
        return;
      }
      const auto ack_bit =
          static_cast<std::uint64_t>(parsed.has_ack && parsed.ack);
      accumulator ^= parsed.request_id_hash ^ ack_bit;
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
        "gate_submit_response_parse_order_place_ack_echo_yyjson_default_padded_"
        "view")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkOrderPlaceAckEchoYyjsonAckMinimalPaddedView)
    ->Name(
        "gate_submit_response_parse_order_place_ack_echo_yyjson_ack_minimal_"
        "padded_view")
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
        "gate_submit_response_parse_order_place_ack_echo_yyjson_insitu_copy_"
        "pool")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkOrderPlaceAckEchoSimdjsonOnDemandCopy)
    ->Name(
        "gate_submit_response_parse_order_place_ack_echo_simdjson_ondemand_"
        "copy")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkOrderPlaceAckEchoSimdjsonOnDemandPaddedView)
    ->Name(
        "gate_submit_response_parse_order_place_ack_echo_simdjson_ondemand_"
        "padded_view")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BenchmarkOrderPlaceAckEchoSimdjsonAckMinimalPaddedView)
    ->Name(
        "gate_submit_response_parse_order_place_ack_echo_simdjson_ack_minimal_"
        "padded_view")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
