#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

namespace aq_binance = aquila::binance;
namespace ws = aquila::websocket;
namespace ws_bench = aquila::websocket::benchmarking;

namespace {

constexpr std::int64_t kLocalNs = 4'720'000'000'000'000;
constexpr std::string_view kBookTickerJson =
    R"({"e":"bookTicker","u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";

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

void BenchmarkParseBookTicker(benchmark::State& state) {
  simdjson::ondemand::parser parser;
  aq_binance::BookTickerUpdate update{};
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
  aq_binance::BookTickerUpdate update{};
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
