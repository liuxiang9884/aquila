#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include "benchmark/strategy/lead_lag_benchmark_support.h"
#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/config/strategy_config.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_feedback_shm.h"
#include "core/trading/order_types.h"
#include "core/trading/trading_runtime.h"
#include "evaluation/exchange/gate/trading/order_feedback_payload_builder.h"
#include "exchange/bitget/trading/order_feedback_parser.h"
#include "exchange/gate/trading/order_codecs.h"
#include "exchange/gate/trading/order_feedback_parser.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/strategy.h"
#include <simdjson.h>

namespace aquila::strategy::leadlag {
namespace {

constexpr std::uint8_t kStrategyId = 4;
constexpr std::uint64_t kReaderRunId = 0xA110'0001ULL;
constexpr std::int32_t kSymbolId = 3;
constexpr std::int64_t kFeedbackLocalReceiveNs = 201;
constexpr std::size_t kOrderCapacity = 8;
constexpr std::size_t kLatencyIterations = 4096;

struct SharedOrderSessionState {
  std::uint64_t place_calls{0};
  std::uint64_t last_place_local_order_id{0};
};

struct BenchmarkOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
  };

  explicit BenchmarkOrderSession(SharedOrderSessionState* shared_state)
      : state(shared_state) {}

  [[nodiscard]] bool Start() noexcept {
    running = true;
    return true;
  }

  void Stop() noexcept { running = false; }

  [[nodiscard]] bool Ready() const noexcept { return true; }

  [[nodiscard]] bool Running() const noexcept { return running; }

  SendResult PlaceOrder(core::StrategyOrder& order) noexcept {
    if (state != nullptr) {
      ++state->place_calls;
      state->last_place_local_order_id = order.local_order_id;
    }
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(core::StrategyOrder&) noexcept {
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk};
  }

  SharedOrderSessionState* state{};
  bool running{false};
};

using Runtime =
    core::TradingRuntime<Strategy, BenchmarkOrderSession,
                         market_data::RealtimeDataReader<>,
                         core::TradingRuntimeDiagnostics>;

[[nodiscard]] config::StrategyConfig RuntimeConfig() {
  config::StrategyConfig config;
  config.name = "lead_lag";
  config.strategy_id = kStrategyId;
  config.order_capacity = kOrderCapacity;
  config.feedback.enabled = false;
  return config;
}

[[nodiscard]] Config BenchmarkConfig(Exchange lag_exchange) {
  const std::string_view exchange_symbol =
      lag_exchange == Exchange::kBitget ? "BTCUSDT" : "BTC_USDT";
  Config config;
  config.name = "lead_lag";
  config.version = "1.0";
  config.pairs.push_back(PairConfig{
      .symbol = "BTC_USDT",
      .symbol_id = kSymbolId,
      .lead_exchange = Exchange::kBinance,
      .lag_exchange = lag_exchange,
      .lag_taker_fee = 0.0,
      .max_lead_freshness_ms = benchmarking::kWideFreshnessGuardMs,
      .max_lag_freshness_ms = benchmarking::kWideFreshnessGuardMs,
      .trigger =
          TriggerConfig{
              .lead = 0.02,
              .close = 0.005,
              .lag_part = 0.5,
              .target_profit_rate = 0.0,
              .drift_period_ns = 1'000'000'000ULL,
              .drift_min_samples = 1,
              .drift_warmup_ns = 1,
              .quantile =
                  QuantileConfig{
                      .move = 0.75,
                      .up_min = 0.0,
                      .up_max = 0.10,
                      .down_min = -0.10,
                      .down_max = 0.0,
                      .precision = 0.000001,
                  },
          },
      .execute =
          ExecuteConfig{
              .open_notional = 1000.0,
              .trailing_stop = 0.05,
              .max_entry_spread = 0.02,
              .parallel = 1,
          },
      .bbo_record =
          BboRecordConfig{
              .window_ns = 1'000'000'000ULL,
              .stats_window_ns = 1'000'000'000ULL,
          },
      .lag_instrument =
          InstrumentMetadata{
              .symbol_id = kSymbolId,
              .exchange = lag_exchange,
              .exchange_symbol = std::string(exchange_symbol),
              .price_tick = 0.1,
              .price_decimal_places = 1,
              .quantity_step = 1.0,
              .quantity_decimal_places = 0,
              .min_quantity = 1.0,
              .max_quantity = 20.0,
              .notional_multiplier = 1.0,
              .lag_taker_fee = 0.0,
          },
  });
  return config;
}

[[nodiscard]] BookTicker Ticker(Exchange exchange, std::int64_t local_ns,
                                double bid_price,
                                double ask_price) noexcept {
  return BookTicker{
      .id = local_ns,
      .symbol_id = kSymbolId,
      .exchange = exchange,
      .exchange_ns = local_ns - 10,
      .local_ns = local_ns,
      .bid_price = bid_price,
      .bid_volume = 1.0,
      .ask_price = ask_price,
      .ask_volume = 1.0,
  };
}

[[nodiscard]] bool SeedPendingOpenOrder(Runtime& runtime,
                                        SharedOrderSessionState& state,
                                        Exchange lag_exchange) {
  const auto base_ns =
      benchmarking::RealtimeNowNs();
  runtime.HandleBookTickerForTest(
      Ticker(lag_exchange, base_ns, 101.57, 102.02));
  runtime.HandleBookTickerForTest(
      Ticker(Exchange::kBinance, base_ns + 1'000, 100.0, 101.0));
  runtime.HandleBookTickerForTest(
      Ticker(Exchange::kBinance, base_ns + 2'000, 112.0, 113.0));
  return state.place_calls == 1 && state.last_place_local_order_id != 0;
}

[[nodiscard]] std::unique_ptr<OrderFeedbackShmChannel> MakeChannel() {
  auto channel = std::make_unique<OrderFeedbackShmChannel>();
  order_feedback_shm_detail::InitializeLaneHeaders(*channel);
  return channel;
}

[[nodiscard]] std::string_view FilledFeedbackPayload(
    std::uint64_t local_order_id, std::array<char, 512>* payload_buffer,
    std::array<char, 64>* text_buffer) noexcept {
  const std::string_view text =
      gate::OrderTextCodec::Format(local_order_id, *text_buffer);
  gate::evaluation::OrderFeedbackPayloadFields fields{};
  fields.exchange_order_id = local_order_id + 1000;
  fields.left_mantissa = 0;
  fields.size_mantissa = 7;
  fields.update_time_us = 200;
  fields.fill_price_mantissa = 10'210;
  fields.role = "taker";
  fields.text = text;
  fields.finish_as = "filled";
  return gate::evaluation::BuildOrderFeedbackOrdersPayload(payload_buffer,
                                                           fields);
}

[[nodiscard]] std::string_view BitgetFilledFeedbackPayload(
    std::uint64_t local_order_id,
    std::array<char, 512 + simdjson::SIMDJSON_PADDING>*
        payload_buffer) noexcept {
  const auto result = fmt::format_to_n(
      payload_buffer->data(),
      payload_buffer->size() - simdjson::SIMDJSON_PADDING,
      R"({{"action":"snapshot","arg":{{"instType":"UTA","topic":"order"}},"data":[{{"category":"usdt-futures","orderId":"{}","clientOid":"a-{}","qty":"7","holdMode":"one_way_mode","marginMode":"crossed","cumExecQty":"7","avgPrice":"102.10","orderStatus":"filled","updatedTime":"200"}}]}})",
      local_order_id + 1000, local_order_id);
  if (result.size > payload_buffer->size() - simdjson::SIMDJSON_PADDING) {
    return {};
  }
  return {payload_buffer->data(), result.size};
}

void BM_LeadLagFeedbackParserShmToRuntimeTerminalFillLatency(
    benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  auto channel = MakeChannel();
  OrderFeedbackShmPublisher publisher(*channel);
  auto reader_result =
      OrderFeedbackShmReader::Claim(*channel, kStrategyId, kReaderRunId);
  if (!reader_result.ok) {
    state.SkipWithError(reader_result.error.c_str());
    return;
  }

  for (auto _ : state) {
    state.PauseTiming();
    SharedOrderSessionState session_state;
    auto runtime_result = Runtime::CreateForTest(
        RuntimeConfig(), [&session_state] {
          return BenchmarkOrderSession{&session_state};
        },
        BenchmarkConfig(Exchange::kGate));
    if (!runtime_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(runtime_result.error.c_str());
      return;
    }

    Runtime& runtime = *runtime_result.value;
    if (!SeedPendingOpenOrder(runtime, session_state, Exchange::kGate)) {
      state.ResumeTiming();
      state.SkipWithError("failed to seed pending lead lag order");
      return;
    }

    std::array<char, 512> payload_buffer{};
    std::array<char, 64> text_buffer{};
    const std::string_view payload = FilledFeedbackPayload(
        session_state.last_place_local_order_id, &payload_buffer, &text_buffer);
    gate::OrderFeedbackParserStats parser_stats{};
    std::uint64_t observed_local_order_id = 0;
    bool published = false;

    state.ResumeTiming();
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const gate::OrderFeedbackParseResult parsed =
        gate::ParseGateOrderFeedbackMessage(
            payload, kFeedbackLocalReceiveNs, parser_stats,
            [&publisher, &published](const OrderFeedbackEvent& event) noexcept {
              published = publisher.Publish(event);
            });
    const std::size_t polled = reader_result.value.Poll(
        1, [&runtime, &observed_local_order_id](
               const OrderFeedbackEvent& polled_event) {
          observed_local_order_id = polled_event.local_order_id;
          runtime.HandleOrderFeedbackForTest(polled_event);
        });
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.PauseTiming();

    if (parsed.status != gate::OrderFeedbackParseStatus::kOk ||
        parsed.events_emitted != 1 || !published || polled != 1 ||
        observed_local_order_id != session_state.last_place_local_order_id) {
      state.ResumeTiming();
      state.SkipWithError("feedback parser/shm/runtime path failed");
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    benchmark::DoNotOptimize(observed_local_order_id);
    benchmark::DoNotOptimize(reader_result.value.consumed_count());
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "feedback_events", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

void BM_LeadLagBitgetFeedbackParserShmToRuntimeTerminalFillLatency(
    benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  auto channel = MakeChannel();
  OrderFeedbackShmPublisher publisher(*channel);
  auto reader_result =
      OrderFeedbackShmReader::Claim(*channel, kStrategyId, kReaderRunId);
  if (!reader_result.ok) {
    state.SkipWithError(reader_result.error.c_str());
    return;
  }
  simdjson::ondemand::parser parser;

  for (auto _ : state) {
    state.PauseTiming();
    SharedOrderSessionState session_state;
    auto runtime_result = Runtime::CreateForTest(
        RuntimeConfig(),
        [&session_state] { return BenchmarkOrderSession{&session_state}; },
        BenchmarkConfig(Exchange::kBitget));
    if (!runtime_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(runtime_result.error.c_str());
      return;
    }

    Runtime& runtime = *runtime_result.value;
    if (!SeedPendingOpenOrder(runtime, session_state, Exchange::kBitget)) {
      state.ResumeTiming();
      state.SkipWithError("failed to seed pending Bitget lead lag order");
      return;
    }

    std::array<char, 512 + simdjson::SIMDJSON_PADDING> payload_buffer{};
    const std::string_view payload = BitgetFilledFeedbackPayload(
        session_state.last_place_local_order_id, &payload_buffer);
    if (payload.empty()) {
      state.ResumeTiming();
      state.SkipWithError("failed to build Bitget feedback payload");
      return;
    }
    bitget::OrderFeedbackParserStats parser_stats{};
    std::uint64_t observed_local_order_id = 0;
    bool published = false;

    state.ResumeTiming();
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const bitget::OrderFeedbackParseResult parsed =
        bitget::ParseBitgetOrderFeedbackMessage(
            payload, simdjson::SIMDJSON_PADDING, kFeedbackLocalReceiveNs,
            parser, parser_stats,
            [&publisher, &published](const OrderFeedbackEvent& event) noexcept {
              published = publisher.Publish(event);
            });
    const std::size_t polled = reader_result.value.Poll(
        1, [&runtime,
            &observed_local_order_id](const OrderFeedbackEvent& polled_event) {
          observed_local_order_id = polled_event.local_order_id;
          runtime.HandleOrderFeedbackForTest(polled_event);
        });
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.PauseTiming();

    if (parsed.status != bitget::OrderFeedbackParseStatus::kOk ||
        parsed.events_emitted != 1 || !published || polled != 1 ||
        observed_local_order_id != session_state.last_place_local_order_id) {
      state.ResumeTiming();
      state.SkipWithError("Bitget feedback parser/shm/runtime path failed");
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    benchmark::DoNotOptimize(observed_local_order_id);
    benchmark::DoNotOptimize(reader_result.value.consumed_count());
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "feedback_events", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_LeadLagFeedbackParserShmToRuntimeTerminalFillLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagBitgetFeedbackParserShmToRuntimeTerminalFillLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila::strategy::leadlag
