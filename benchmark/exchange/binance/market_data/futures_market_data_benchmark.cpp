#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "benchmark/websocket/benchmark_support.h"
#include "core/websocket/message_view.h"
#include "exchange/binance/market_data/book_ticker_parser.h"
#include "exchange/binance/market_data/client.h"
#include "exchange/binance/market_data/session.h"
#include <simdjson.h>
#include <yyjson.h>

namespace aq_binance = aquila::binance;
namespace ws = aquila::websocket;
namespace ws_bench = aquila::websocket::benchmarking;

namespace {

constexpr std::int64_t kLocalNs = 4'720'000'000'000'000;
constexpr size_t kYyjsonBookTickerReadPoolBytes = 4096;
constexpr size_t kYyjsonInsituViewIterations = 100'000;
constexpr std::string_view kBookTickerJson =
    R"({"e":"bookTicker","u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";
constexpr size_t kYyjsonMutablePayloadStride =
    kBookTickerJson.size() + YYJSON_PADDING_SIZE;

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

bool ReadYyjsonString(yyjson_val* value, std::string_view* output) noexcept {
  if (output == nullptr || !yyjson_is_str(value)) {
    return false;
  }
  const char* const text = yyjson_get_str(value);
  if (text == nullptr) {
    return false;
  }
  *output = std::string_view(text, yyjson_get_len(value));
  return true;
}

bool ReadYyjsonInt64(yyjson_val* value, std::int64_t* output) noexcept {
  if (output == nullptr) {
    return false;
  }
  if (yyjson_is_uint(value)) {
    const std::uint64_t parsed = yyjson_get_uint(value);
    if (parsed >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    *output = static_cast<std::int64_t>(parsed);
    return true;
  }
  if (yyjson_is_sint(value)) {
    *output = yyjson_get_sint(value);
    return true;
  }
  return false;
}

aq_binance::BookTickerParseStatus ParseYyjsonBookTickerDocument(
    yyjson_doc* doc, aq_binance::BookTickerUpdate* output) noexcept {
  if (doc == nullptr) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }
  if (output == nullptr) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }

  yyjson_val* root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }

  std::int64_t update_id = 0;
  if (!ReadYyjsonInt64(yyjson_obj_get(root, "u"), &update_id)) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }

  std::int64_t event_time_ms = 0;
  if (!ReadYyjsonInt64(yyjson_obj_get(root, "E"), &event_time_ms)) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }

  std::string_view symbol;
  if (!ReadYyjsonString(yyjson_obj_get(root, "s"), &symbol)) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }

  std::string_view number;
  if (!ReadYyjsonString(yyjson_obj_get(root, "b"), &number)) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }
  const double bid_price = aq_binance::detail::ParseDoubleString(number);

  if (!ReadYyjsonString(yyjson_obj_get(root, "B"), &number)) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }
  const double bid_volume = aq_binance::detail::ParseDoubleString(number);

  if (!ReadYyjsonString(yyjson_obj_get(root, "a"), &number)) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }
  const double ask_price = aq_binance::detail::ParseDoubleString(number);

  if (!ReadYyjsonString(yyjson_obj_get(root, "A"), &number)) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }
  const double ask_volume = aq_binance::detail::ParseDoubleString(number);

  output->update_id = update_id;
  output->event_time_ms = event_time_ms;
  output->bid_price = bid_price;
  output->bid_volume = bid_volume;
  output->ask_price = ask_price;
  output->ask_volume = ask_volume;
  aq_binance::detail::CopySymbolToStorage(symbol, *output);
  return aq_binance::BookTickerParseStatus::kOk;
}

template <size_t ReadPoolBytes = kYyjsonBookTickerReadPoolBytes>
class BasicYyjsonBookTickerParser {
 public:
  aq_binance::BookTickerParseStatus Parse(
      std::string_view payload, std::uint32_t readable_tail_bytes,
      aq_binance::BookTickerUpdate& output) noexcept {
    (void)readable_tail_bytes;
    if (payload.empty()) {
      return aq_binance::BookTickerParseStatus::kMalformedJson;
    }

    return ParseWithFlags(const_cast<char*>(payload.data()), payload.size(),
                          YYJSON_READ_NOFLAG, output);
  }

  aq_binance::BookTickerParseStatus ParseInsitu(
      std::span<char> padded_payload, size_t payload_size,
      aq_binance::BookTickerUpdate& output) noexcept {
    if (payload_size == 0 || payload_size > padded_payload.size() ||
        padded_payload.size() - payload_size < YYJSON_PADDING_SIZE) {
      return aq_binance::BookTickerParseStatus::kMalformedJson;
    }

    return ParseWithFlags(padded_payload.data(), payload_size,
                          YYJSON_READ_INSITU, output);
  }

 private:
  aq_binance::BookTickerParseStatus ParseWithFlags(
      char* payload, size_t payload_size, yyjson_read_flag flags,
      aq_binance::BookTickerUpdate& output) noexcept {
    yyjson_alc allocator{};
    if (!yyjson_alc_pool_init(&allocator, read_pool_.data(),
                              read_pool_.size())) {
      return aq_binance::BookTickerParseStatus::kMalformedJson;
    }

    yyjson_read_err error{};
    YyjsonDoc doc(
        yyjson_read_opts(payload, payload_size, flags, &allocator, &error));
    if (doc.get() == nullptr) {
      return aq_binance::BookTickerParseStatus::kMalformedJson;
    }
    return ParseYyjsonBookTickerDocument(doc.get(), &output);
  }

  alignas(std::max_align_t) std::array<char, ReadPoolBytes> read_pool_{};
};

using YyjsonBookTickerParser = BasicYyjsonBookTickerParser<>;

template <size_t ReadPoolBytes = kYyjsonBookTickerReadPoolBytes>
class BasicYyjsonInsituBookTickerParser {
 public:
  aq_binance::BookTickerParseStatus Parse(
      std::string_view payload, std::uint32_t readable_tail_bytes,
      aq_binance::BookTickerUpdate& output) noexcept {
    if (payload.empty()) {
      return aq_binance::BookTickerParseStatus::kMalformedJson;
    }
    if (readable_tail_bytes >= YYJSON_PADDING_SIZE) {
      return parser_.ParseInsitu(
          std::span<char>(const_cast<char*>(payload.data()),
                          payload.size() + readable_tail_bytes),
          payload.size(), output);
    }
    return parser_.Parse(payload, readable_tail_bytes, output);
  }

 private:
  BasicYyjsonBookTickerParser<ReadPoolBytes> parser_;
};

using YyjsonInsituBookTickerParser = BasicYyjsonInsituBookTickerParser<>;

simdjson::ondemand::value OrderedField(simdjson::ondemand::object& object,
                                       std::string_view key) noexcept {
  simdjson::simdjson_result<simdjson::ondemand::value> result =
      object.find_field(key);
  assert(result.error() == simdjson::SUCCESS);
  return result.value_unsafe();
}

aq_binance::BookTickerParseStatus ParseOrderedBookTickerObject(
    simdjson::ondemand::object root,
    aq_binance::BookTickerUpdate& output) noexcept {
  const std::int64_t update_id =
      aq_binance::detail::Int64Value(OrderedField(root, "u"));
  const std::int64_t event_time_ms =
      aq_binance::detail::Int64Value(OrderedField(root, "E"));
  const std::string_view symbol =
      aq_binance::detail::StringValue(OrderedField(root, "s"));
  const double bid_price = aq_binance::detail::ParseDoubleString(
      aq_binance::detail::StringValue(OrderedField(root, "b")));
  const double bid_volume = aq_binance::detail::ParseDoubleString(
      aq_binance::detail::StringValue(OrderedField(root, "B")));
  const double ask_price = aq_binance::detail::ParseDoubleString(
      aq_binance::detail::StringValue(OrderedField(root, "a")));
  const double ask_volume = aq_binance::detail::ParseDoubleString(
      aq_binance::detail::StringValue(OrderedField(root, "A")));

  output.update_id = update_id;
  output.event_time_ms = event_time_ms;
  output.bid_price = bid_price;
  output.bid_volume = bid_volume;
  output.ask_price = ask_price;
  output.ask_volume = ask_volume;
  aq_binance::detail::CopySymbolToStorage(symbol, output);
  return aq_binance::BookTickerParseStatus::kOk;
}

aq_binance::BookTickerParseStatus ParseOrderedBookTickerDocument(
    simdjson::ondemand::document document,
    aq_binance::BookTickerUpdate& output) noexcept {
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }
  return ParseOrderedBookTickerObject(root, output);
}

aq_binance::BookTickerParseStatus ParseOrderedBookTicker(
    std::string_view payload, std::uint32_t readable_tail_bytes,
    simdjson::ondemand::parser& parser,
    aq_binance::BookTickerUpdate& output) noexcept {
  if (payload.empty()) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }

  simdjson::ondemand::document document;
  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    simdjson::padded_string_view view(payload.data(), payload.size(),
                                      payload.size() + readable_tail_bytes);
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      return aq_binance::BookTickerParseStatus::kMalformedJson;
    }
    return ParseOrderedBookTickerDocument(std::move(document), output);
  }

  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return aq_binance::BookTickerParseStatus::kMalformedJson;
  }
  return ParseOrderedBookTickerDocument(std::move(document), output);
}

ws::MessageView TextView(std::string_view payload,
                         std::uint32_t readable_tail_bytes = 0) noexcept {
  return {
      .kind = ws::PayloadKind::kText,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 8,
      .fin = true,
      .readable_tail_bytes = readable_tail_bytes,
  };
}

ws::MessageView BinaryView(std::string_view payload) noexcept {
  return {
      .kind = ws::PayloadKind::kBinary,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 9,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

struct CountingConsumer {
  std::uint64_t calls{0};
  std::uint64_t id_xor{0};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++calls;
    id_xor ^= static_cast<std::uint64_t>(book_ticker.id);
  }
};

struct SymbolSet {
  std::vector<std::string> storage;
  std::vector<aq_binance::SymbolBinding> bindings;
  std::string_view target_symbol;
};

SymbolSet BuildSymbols(size_t symbol_count) {
  SymbolSet set;
  set.storage.reserve(symbol_count);
  set.bindings.reserve(symbol_count);
  for (size_t i = 0; i < symbol_count; ++i) {
    if (i + 1 == symbol_count) {
      set.storage.emplace_back("BTCUSDT");
    } else {
      set.storage.emplace_back("SYM" + std::to_string(i) + "USDT");
    }
  }
  for (size_t i = 0; i < symbol_count; ++i) {
    set.bindings.push_back({.symbol = set.storage[i],
                            .symbol_id = static_cast<std::int32_t>(1000 + i)});
  }
  set.target_symbol = set.bindings.back().symbol;
  return set;
}

ws::ConnectionConfig BuildConnectionConfig() {
  ws::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "443";
  return config;
}

void SetCommonCounters(benchmark::State& state, std::string_view payload) {
  state.counters["payload_bytes"] = static_cast<double>(payload.size());
  state.SetLabel(ws_bench::BuildBenchmarkLabel(
      false, "binance-futures-book-ticker", ws_bench::FormatAffinity(),
      ws_bench::FormatSchedulingPolicy()));
}

std::vector<char> BuildMutableBookTickerPayloads(size_t payload_count) {
  std::vector<char> payloads(payload_count * kYyjsonMutablePayloadStride);
  for (size_t i = 0; i < payload_count; ++i) {
    std::memcpy(payloads.data() + i * kYyjsonMutablePayloadStride,
                kBookTickerJson.data(), kBookTickerJson.size());
  }
  return payloads;
}

std::string_view MutableBookTickerPayload(std::vector<char>& payloads,
                                          size_t index) noexcept {
  return {payloads.data() + index * kYyjsonMutablePayloadStride,
          kBookTickerJson.size()};
}

void BenchmarkParseBookTicker(benchmark::State& state) {
  simdjson::ondemand::parser parser;
  aq_binance::BookTickerUpdate update;
  std::uint64_t parsed = 0;

  for (auto _ : state) {
    aq_binance::BookTickerParseStatus status =
        aq_binance::ParseBookTicker(kBookTickerJson, 0, parser, update);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(update);
    parsed += static_cast<std::uint64_t>(
        status == aq_binance::BookTickerParseStatus::kOk);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(parsed));
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkParseBookTicker)
    ->Name("binance_market_data/parse_book_ticker")
    ->Unit(benchmark::kNanosecond);

void BenchmarkParseBookTickerPaddedView(benchmark::State& state) {
  std::array<char, kBookTickerJson.size() + simdjson::SIMDJSON_PADDING>
      scratch{};
  std::memcpy(scratch.data(), kBookTickerJson.data(), kBookTickerJson.size());
  simdjson::ondemand::parser parser;
  aq_binance::BookTickerUpdate update;
  std::uint64_t parsed = 0;

  for (auto _ : state) {
    aq_binance::BookTickerParseStatus status = aq_binance::ParseBookTicker(
        std::string_view(scratch.data(), kBookTickerJson.size()),
        static_cast<std::uint32_t>(simdjson::SIMDJSON_PADDING), parser, update);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(update);
    parsed += static_cast<std::uint64_t>(
        status == aq_binance::BookTickerParseStatus::kOk);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(parsed));
  state.counters["scratch_bytes"] = static_cast<double>(scratch.size());
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkParseBookTickerPaddedView)
    ->Name("binance_market_data/parse_book_ticker_padded_view")
    ->Unit(benchmark::kNanosecond);

void BenchmarkParseBookTickerOrdered(benchmark::State& state) {
  simdjson::ondemand::parser parser;
  aq_binance::BookTickerUpdate update;
  std::uint64_t parsed = 0;

  for (auto _ : state) {
    aq_binance::BookTickerParseStatus status =
        ParseOrderedBookTicker(kBookTickerJson, 0, parser, update);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(update);
    parsed += static_cast<std::uint64_t>(
        status == aq_binance::BookTickerParseStatus::kOk);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(parsed));
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkParseBookTickerOrdered)
    ->Name("binance_market_data/parse_book_ticker_ordered")
    ->Unit(benchmark::kNanosecond);

void BenchmarkParseBookTickerOrderedPaddedView(benchmark::State& state) {
  std::array<char, kBookTickerJson.size() + simdjson::SIMDJSON_PADDING>
      scratch{};
  std::memcpy(scratch.data(), kBookTickerJson.data(), kBookTickerJson.size());
  simdjson::ondemand::parser parser;
  aq_binance::BookTickerUpdate update;
  std::uint64_t parsed = 0;

  for (auto _ : state) {
    aq_binance::BookTickerParseStatus status = ParseOrderedBookTicker(
        std::string_view(scratch.data(), kBookTickerJson.size()),
        static_cast<std::uint32_t>(simdjson::SIMDJSON_PADDING), parser, update);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(update);
    parsed += static_cast<std::uint64_t>(
        status == aq_binance::BookTickerParseStatus::kOk);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(parsed));
  state.counters["scratch_bytes"] = static_cast<double>(scratch.size());
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkParseBookTickerOrderedPaddedView)
    ->Name("binance_market_data/parse_book_ticker_ordered_padded_view")
    ->Unit(benchmark::kNanosecond);

void BenchmarkParseBookTickerYyjsonPool(benchmark::State& state) {
  YyjsonBookTickerParser parser;
  aq_binance::BookTickerUpdate update;
  std::uint64_t parsed = 0;

  for (auto _ : state) {
    aq_binance::BookTickerParseStatus status =
        parser.Parse(kBookTickerJson, 0, update);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(update);
    parsed += static_cast<std::uint64_t>(
        status == aq_binance::BookTickerParseStatus::kOk);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(parsed));
  state.counters["yyjson_pool_bytes"] =
      static_cast<double>(kYyjsonBookTickerReadPoolBytes);
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkParseBookTickerYyjsonPool)
    ->Name("binance_market_data/parse_book_ticker_yyjson_pool")
    ->Unit(benchmark::kNanosecond);

void BenchmarkParseBookTickerYyjsonInsituCopy(benchmark::State& state) {
  std::array<char, kBookTickerJson.size() + YYJSON_PADDING_SIZE> scratch{};
  YyjsonBookTickerParser parser;
  aq_binance::BookTickerUpdate update;
  std::uint64_t parsed = 0;

  for (auto _ : state) {
    std::memcpy(scratch.data(), kBookTickerJson.data(), kBookTickerJson.size());
    aq_binance::BookTickerParseStatus status =
        parser.ParseInsitu(std::span<char>(scratch.data(), scratch.size()),
                           kBookTickerJson.size(), update);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(update);
    parsed += static_cast<std::uint64_t>(
        status == aq_binance::BookTickerParseStatus::kOk);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(parsed));
  state.counters["scratch_bytes"] = static_cast<double>(scratch.size());
  state.counters["yyjson_padding_bytes"] =
      static_cast<double>(YYJSON_PADDING_SIZE);
  state.counters["yyjson_pool_bytes"] =
      static_cast<double>(kYyjsonBookTickerReadPoolBytes);
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkParseBookTickerYyjsonInsituCopy)
    ->Name("binance_market_data/parse_book_ticker_yyjson_insitu_copy")
    ->Unit(benchmark::kNanosecond);

void BenchmarkParseBookTickerYyjsonInsituView(benchmark::State& state) {
  std::vector<char> payloads =
      BuildMutableBookTickerPayloads(kYyjsonInsituViewIterations);
  YyjsonInsituBookTickerParser parser;
  aq_binance::BookTickerUpdate update;
  std::uint64_t parsed = 0;
  size_t index = 0;

  for (auto _ : state) {
    const std::string_view payload = MutableBookTickerPayload(payloads, index);
    aq_binance::BookTickerParseStatus status =
        parser.Parse(payload, YYJSON_PADDING_SIZE, update);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(update);
    parsed += static_cast<std::uint64_t>(
        status == aq_binance::BookTickerParseStatus::kOk);
    ++index;
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(parsed));
  state.counters["scratch_bytes"] =
      static_cast<double>(kYyjsonMutablePayloadStride);
  state.counters["yyjson_padding_bytes"] =
      static_cast<double>(YYJSON_PADDING_SIZE);
  state.counters["yyjson_pool_bytes"] =
      static_cast<double>(kYyjsonBookTickerReadPoolBytes);
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkParseBookTickerYyjsonInsituView)
    ->Name("binance_market_data/parse_book_ticker_yyjson_insitu_view")
    ->Iterations(kYyjsonInsituViewIterations)
    ->Unit(benchmark::kNanosecond);

void BenchmarkClientOnTextPayload(benchmark::State& state) {
  const size_t symbol_count = static_cast<size_t>(state.range(0));
  SymbolSet symbols = BuildSymbols(symbol_count);
  CountingConsumer consumer;
  aq_binance::FuturesMarketDataClient client(
      std::span<const aq_binance::SymbolBinding>(symbols.bindings), consumer);

  for (auto _ : state) {
    ws::DeliveryResult result =
        client.OnTextPayload(kBookTickerJson, 0, kLocalNs);
    benchmark::DoNotOptimize(result);
  }

  benchmark::DoNotOptimize(consumer);
  state.SetItemsProcessed(static_cast<std::int64_t>(consumer.calls));
  state.counters["symbols"] = static_cast<double>(symbol_count);
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkClientOnTextPayload)
    ->Name("binance_market_data/client_on_text_payload")
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Unit(benchmark::kNanosecond);

void BenchmarkClientHandleBinary(benchmark::State& state) {
  SymbolSet symbols = BuildSymbols(1);
  CountingConsumer consumer;
  aq_binance::FuturesMarketDataClient client(
      std::span<const aq_binance::SymbolBinding>(symbols.bindings), consumer);
  const ws::MessageView view = BinaryView(kBookTickerJson);
  std::uint64_t accepted = 0;

  for (auto _ : state) {
    ws::DeliveryResult result = client.Handle(view);
    benchmark::DoNotOptimize(result);
    accepted +=
        static_cast<std::uint64_t>(result == ws::DeliveryResult::kAccepted);
  }

  benchmark::DoNotOptimize(consumer);
  state.SetItemsProcessed(static_cast<std::int64_t>(accepted));
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkClientHandleBinary)
    ->Name("binance_market_data/client_handle_binary")
    ->Unit(benchmark::kNanosecond);

void BenchmarkSessionHandleText(benchmark::State& state) {
  const size_t symbol_count = static_cast<size_t>(state.range(0));
  SymbolSet symbols = BuildSymbols(symbol_count);
  CountingConsumer consumer;
  aq_binance::FuturesMarketDataSession<CountingConsumer, ws::PlainSocket>
      session(BuildConnectionConfig(),
              std::span<const aq_binance::SymbolBinding>(symbols.bindings),
              consumer);
  const ws::MessageView view = TextView(kBookTickerJson);

  for (auto _ : state) {
    ws::DeliveryResult result = session.Handle(view);
    benchmark::DoNotOptimize(result);
  }

  benchmark::DoNotOptimize(consumer);
  state.SetItemsProcessed(static_cast<std::int64_t>(consumer.calls));
  state.counters["symbols"] = static_cast<double>(symbol_count);
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkSessionHandleText)
    ->Name("binance_market_data/session_handle_text")
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Unit(benchmark::kNanosecond);

void BenchmarkSessionHandleTextPaddedView(benchmark::State& state) {
  std::array<char, kBookTickerJson.size() + simdjson::SIMDJSON_PADDING>
      scratch{};
  std::memcpy(scratch.data(), kBookTickerJson.data(), kBookTickerJson.size());
  SymbolSet symbols = BuildSymbols(1);
  CountingConsumer consumer;
  aq_binance::FuturesMarketDataSession<CountingConsumer, ws::PlainSocket>
      session(BuildConnectionConfig(),
              std::span<const aq_binance::SymbolBinding>(symbols.bindings),
              consumer);
  const ws::MessageView view =
      TextView({scratch.data(), kBookTickerJson.size()},
               static_cast<std::uint32_t>(simdjson::SIMDJSON_PADDING));

  for (auto _ : state) {
    ws::DeliveryResult result = session.Handle(view);
    benchmark::DoNotOptimize(result);
  }

  benchmark::DoNotOptimize(consumer);
  state.SetItemsProcessed(static_cast<std::int64_t>(consumer.calls));
  state.counters["scratch_bytes"] = static_cast<double>(scratch.size());
  SetCommonCounters(state, kBookTickerJson);
}

BENCHMARK(BenchmarkSessionHandleTextPaddedView)
    ->Name("binance_market_data/session_handle_text_padded_view")
    ->Unit(benchmark::kNanosecond);

}  // namespace
