#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include "benchmark/websocket/benchmark_support.h"
#include "core/market_data/data_shm.h"
#include "core/websocket/message_view.h"
#include "evaluation/exchange/gate/sbe/book_ticker_payload_builder.h"
#include "exchange/gate/market_data/client.h"
#include "exchange/gate/market_data/data_session.h"
#include "exchange/gate/sbe/book_ticker_decoder.h"
#include <simdjson.h>

namespace aq_gate = aquila::gate;
namespace aq_md = aquila::market_data;
namespace ws = aquila::websocket;
namespace ws_bench = aquila::websocket::benchmarking;

namespace {

constexpr std::int64_t kLocalNs = 4'720'000'000'000'000;

using aquila::gate::evaluation::BuildBookTickerPayload;

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

  std::string shm_name;
};

std::string UniqueShmName(std::string_view suffix) {
  return fmt::format("/aquila_gate_market_data_bench_{}_{}", ::getpid(),
                     suffix);
}

aq_md::BookTickerShmConfig MakeCreateConfig(std::string_view suffix) {
  return aq_md::BookTickerShmConfig{
      .enabled = true,
      .shm_name = UniqueShmName(suffix),
      .channel_name = "book_ticker_channel",
      .create = true,
      .remove_existing = true,
  };
}

ws::MessageView BinaryView(std::string_view payload) noexcept {
  return {
      .kind = ws::PayloadKind::kBinary,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 7,
      .fin = true,
      .readable_tail_bytes = 0,
  };
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

struct CountingConsumer {
  std::uint64_t calls{0};
  std::uint64_t id_xor{0};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++calls;
    id_xor ^= static_cast<std::uint64_t>(book_ticker.id);
  }
};

using InstrumentedPlainSession =
    aq_gate::DataSession<CountingConsumer, aq_gate::DefaultPlainWebSocketPolicy,
                         aq_gate::SessionOnlyDiagnosticsPolicy>;

struct SymbolSet {
  std::vector<std::string> storage;
  std::vector<aq_gate::SymbolBinding> bindings;
  std::string_view target_symbol;
};

SymbolSet BuildSymbols(size_t symbol_count) {
  SymbolSet set;
  set.storage.reserve(symbol_count);
  set.bindings.reserve(symbol_count);
  for (size_t i = 0; i < symbol_count; ++i) {
    if (i + 1 == symbol_count) {
      set.storage.emplace_back("BTC_USDT");
    } else {
      set.storage.emplace_back("SYM" + std::to_string(i) + "_USDT");
    }
  }
  for (size_t i = 0; i < symbol_count; ++i) {
    set.bindings.push_back({.exchange_symbol = set.storage[i],
                            .symbol_id = static_cast<std::int32_t>(1000 + i)});
  }
  set.target_symbol = set.bindings.back().exchange_symbol;
  return set;
}

ws::ConnectionConfig BuildConnectionConfig() {
  ws::ConnectionConfig config{};
  config.host = "localhost";
  config.port = "443";
  config.target = "/v4/ws/usdt/sbe?sbe_schema_id=1";
  config.enable_tls = false;
  return config;
}

void SetCommonCounters(benchmark::State& state, std::string_view payload) {
  state.counters["payload_bytes"] = static_cast<double>(payload.size());
  state.SetLabel(ws_bench::BuildBenchmarkLabel(
      false, "gate-futures-book-ticker", ws_bench::FormatAffinity(),
      ws_bench::FormatSchedulingPolicy()));
}

bool DecodeBookTickerWithHeaderBenchmark(
    std::string_view payload, const aq_gate::SbeMessageHeader& header,
    std::int64_t local_ns, std::int32_t symbol_id,
    aquila::BookTicker* out) noexcept {
  if (out == nullptr ||
      payload.size() < aq_gate::detail::kMinBookTickerPayloadBytes ||
      !aq_gate::detail::IsBookTickerHeader(header)) {
    return false;
  }

  const auto view = ::sbepp::make_const_view<::gate::messages::bbo>(
      payload.data(), payload.size());
  if (view.e() != ::gate::types::Event::Update) {
    return false;
  }

  aq_gate::detail::AssignBookTickerFromView(view, local_ns, symbol_id, *out);
  return true;
}

void BenchmarkDecodeBookTickerWithHeader(benchmark::State& state) {
  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");
  const aq_gate::SbeDispatchResult dispatch =
      aq_gate::DispatchSbeMessage(payload);
  if (dispatch.status != aq_gate::SbeDispatchStatus::kReady ||
      dispatch.message_type != aq_gate::GateSbeMessageType::kBookTicker) {
    state.SkipWithError("dispatch failed");
    return;
  }

  aquila::BookTicker book_ticker;
  std::uint64_t decoded = 0;
  for (auto _ : state) {
    bool ok = DecodeBookTickerWithHeaderBenchmark(payload, dispatch.header,
                                                  kLocalNs, 11, &book_ticker);
    benchmark::DoNotOptimize(ok);
    benchmark::DoNotOptimize(book_ticker);
    decoded += static_cast<std::uint64_t>(ok);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(decoded));
  SetCommonCounters(state, payload);
}

BENCHMARK(BenchmarkDecodeBookTickerWithHeader)
    ->Name("gate_market_data/decode_book_ticker_with_header")
    ->Unit(benchmark::kNanosecond);

void BenchmarkDecodeBookTickerThenShmPush(benchmark::State& state) {
  const aq_md::BookTickerShmConfig config =
      MakeCreateConfig("decode_then_push");
  ShmCleanup cleanup(config.shm_name);
  aq_md::DataShmPublisher publisher(config);
  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");
  const aq_gate::SbeDispatchResult dispatch =
      aq_gate::DispatchSbeMessage(payload);
  if (dispatch.status != aq_gate::SbeDispatchStatus::kReady ||
      dispatch.message_type != aq_gate::GateSbeMessageType::kBookTicker) {
    state.SkipWithError("dispatch failed");
    return;
  }

  std::uint64_t published = 0;
  for (auto _ : state) {
    aquila::BookTicker book_ticker{};
    aq_gate::DecodeBookTickerWithHeader(payload, dispatch.header, kLocalNs, 11,
                                        book_ticker);
    publisher.OnBookTicker(book_ticker);
    ++published;
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(published));
  state.counters["published"] =
      static_cast<double>(publisher.published_count());
  SetCommonCounters(state, payload);
}

BENCHMARK(BenchmarkDecodeBookTickerThenShmPush)
    ->Name("gate_market_data/decode_book_ticker_then_shm_push")
    ->Unit(benchmark::kNanosecond);

void BenchmarkDecodeBookTickerIntoShmSlot(benchmark::State& state) {
  const aq_md::BookTickerShmConfig config =
      MakeCreateConfig("decode_into_slot");
  ShmCleanup cleanup(config.shm_name);
  aq_md::DataShmPublisher publisher(config);
  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");
  const aq_gate::SbeDispatchResult dispatch =
      aq_gate::DispatchSbeMessage(payload);
  if (dispatch.status != aq_gate::SbeDispatchStatus::kReady ||
      dispatch.message_type != aq_gate::GateSbeMessageType::kBookTicker) {
    state.SkipWithError("dispatch failed");
    return;
  }

  std::uint64_t published = 0;
  for (auto _ : state) {
    publisher.EmplaceBookTickerWith([&](aquila::BookTicker& out) noexcept {
      aq_gate::DecodeBookTickerWithHeader(payload, dispatch.header, kLocalNs,
                                          11, out);
    });
    ++published;
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(published));
  state.counters["published"] =
      static_cast<double>(publisher.published_count());
  SetCommonCounters(state, payload);
}

BENCHMARK(BenchmarkDecodeBookTickerIntoShmSlot)
    ->Name("gate_market_data/decode_book_ticker_into_shm_slot")
    ->Unit(benchmark::kNanosecond);

void BenchmarkClientOnMessageBinary(benchmark::State& state) {
  const size_t symbol_count = static_cast<size_t>(state.range(0));
  SymbolSet symbols = BuildSymbols(symbol_count);
  CountingConsumer consumer;
  aq_gate::FuturesMarketDataClient client(
      std::span<const aq_gate::SymbolBinding>(symbols.bindings), consumer);
  std::array<char, 192> buffer{};
  const std::string_view payload =
      BuildBookTickerPayload(&buffer, symbols.target_symbol);
  const ws::MessageView view = BinaryView(payload);

  for (auto _ : state) {
    ws::DeliveryResult result = client.OnMessage(view, kLocalNs);
    benchmark::DoNotOptimize(result);
  }

  benchmark::DoNotOptimize(consumer);
  state.SetItemsProcessed(static_cast<std::int64_t>(consumer.calls));
  state.counters["symbols"] = static_cast<double>(symbol_count);
  SetCommonCounters(state, payload);
}

BENCHMARK(BenchmarkClientOnMessageBinary)
    ->Name("gate_market_data/client_on_message_binary")
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Unit(benchmark::kNanosecond);

void BenchmarkClientOnBinaryPayload(benchmark::State& state) {
  const size_t symbol_count = static_cast<size_t>(state.range(0));
  SymbolSet symbols = BuildSymbols(symbol_count);
  CountingConsumer consumer;
  aq_gate::FuturesMarketDataClient client(
      std::span<const aq_gate::SymbolBinding>(symbols.bindings), consumer);
  std::array<char, 192> buffer{};
  const std::string_view payload =
      BuildBookTickerPayload(&buffer, symbols.target_symbol);
  const ws::MessageView view = BinaryView(payload);

  for (auto _ : state) {
    ws::DeliveryResult result = client.OnBinaryPayload(view.payload, kLocalNs);
    benchmark::DoNotOptimize(result);
  }

  benchmark::DoNotOptimize(consumer);
  state.SetItemsProcessed(static_cast<std::int64_t>(consumer.calls));
  state.counters["symbols"] = static_cast<double>(symbol_count);
  SetCommonCounters(state, payload);
}

BENCHMARK(BenchmarkClientOnBinaryPayload)
    ->Name("gate_market_data/client_on_binary_payload")
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Unit(benchmark::kNanosecond);

void BenchmarkClientHandleBinaryWithClock(benchmark::State& state) {
  const size_t symbol_count = static_cast<size_t>(state.range(0));
  SymbolSet symbols = BuildSymbols(symbol_count);
  CountingConsumer consumer;
  aq_gate::FuturesMarketDataClient client(
      std::span<const aq_gate::SymbolBinding>(symbols.bindings), consumer);
  std::array<char, 192> buffer{};
  const std::string_view payload =
      BuildBookTickerPayload(&buffer, symbols.target_symbol);
  const ws::MessageView view = BinaryView(payload);

  for (auto _ : state) {
    ws::DeliveryResult result = client.Handle(view);
    benchmark::DoNotOptimize(result);
  }

  benchmark::DoNotOptimize(consumer);
  state.SetItemsProcessed(static_cast<std::int64_t>(consumer.calls));
  state.counters["symbols"] = static_cast<double>(symbol_count);
  SetCommonCounters(state, payload);
}

BENCHMARK(BenchmarkClientHandleBinaryWithClock)
    ->Name("gate_market_data/client_handle_binary_with_clock")
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Unit(benchmark::kNanosecond);

void BenchmarkSessionHandleBinary(benchmark::State& state) {
  const size_t symbol_count = static_cast<size_t>(state.range(0));
  SymbolSet symbols = BuildSymbols(symbol_count);
  CountingConsumer consumer;
  aq_gate::DataSession<CountingConsumer, aq_gate::DefaultPlainWebSocketPolicy>
      session(BuildConnectionConfig(),
              std::span<const aq_gate::SymbolBinding>(symbols.bindings),
              consumer);
  std::array<char, 192> buffer{};
  const std::string_view payload =
      BuildBookTickerPayload(&buffer, symbols.target_symbol);
  const ws::MessageView view = BinaryView(payload);

  for (auto _ : state) {
    ws::DeliveryResult result = session.Handle(view);
    benchmark::DoNotOptimize(result);
  }

  benchmark::DoNotOptimize(consumer);
  state.SetItemsProcessed(static_cast<std::int64_t>(consumer.calls));
  state.counters["symbols"] = static_cast<double>(symbol_count);
  SetCommonCounters(state, payload);
}

BENCHMARK(BenchmarkSessionHandleBinary)
    ->Name("gate_market_data/session_handle_binary")
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Unit(benchmark::kNanosecond);

void BenchmarkSessionHandleSubscribeAck(benchmark::State& state) {
  static constexpr std::string_view kSubscribeAck =
      R"({"time":1,"channel":"futures.book_ticker","event":"subscribe","result":{"status":"success"}})";
  SymbolSet symbols = BuildSymbols(1);
  CountingConsumer consumer;
  InstrumentedPlainSession session(
      BuildConnectionConfig(),
      std::span<const aq_gate::SymbolBinding>(symbols.bindings), consumer);
  const ws::MessageView view = TextView(kSubscribeAck);

  for (auto _ : state) {
    ws::DeliveryResult result = session.Handle(view);
    benchmark::DoNotOptimize(result);
  }

  std::uint64_t subscribe_acks = session.stats().subscribe_acks;
  benchmark::DoNotOptimize(subscribe_acks);
  state.SetItemsProcessed(
      static_cast<std::int64_t>(session.stats().subscribe_acks));
  SetCommonCounters(state, kSubscribeAck);
}

BENCHMARK(BenchmarkSessionHandleSubscribeAck)
    ->Name("gate_market_data/session_handle_subscribe_ack")
    ->Unit(benchmark::kNanosecond);

void BenchmarkSessionHandleSubscribeAckPaddedView(benchmark::State& state) {
  static constexpr std::string_view kSubscribeAck =
      R"({"time":1,"channel":"futures.book_ticker","event":"subscribe","result":{"status":"success"}})";
  SymbolSet symbols = BuildSymbols(1);
  CountingConsumer consumer;
  InstrumentedPlainSession session(
      BuildConnectionConfig(),
      std::span<const aq_gate::SymbolBinding>(symbols.bindings), consumer);
  std::array<char, kSubscribeAck.size() + simdjson::SIMDJSON_PADDING> scratch{};
  std::memcpy(scratch.data(), kSubscribeAck.data(), kSubscribeAck.size());
  const ws::MessageView view =
      TextView({scratch.data(), kSubscribeAck.size()},
               static_cast<std::uint32_t>(simdjson::SIMDJSON_PADDING));

  for (auto _ : state) {
    ws::DeliveryResult result = session.Handle(view);
    benchmark::DoNotOptimize(result);
  }

  std::uint64_t subscribe_acks = session.stats().subscribe_acks;
  benchmark::DoNotOptimize(subscribe_acks);
  state.SetItemsProcessed(
      static_cast<std::int64_t>(session.stats().subscribe_acks));
  state.counters["scratch_bytes"] = static_cast<double>(scratch.size());
  SetCommonCounters(state, kSubscribeAck);
}

BENCHMARK(BenchmarkSessionHandleSubscribeAckPaddedView)
    ->Name("gate_market_data/session_handle_subscribe_ack_padded_view")
    ->Unit(benchmark::kNanosecond);

}  // namespace
