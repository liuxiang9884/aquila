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
constexpr std::size_t kActualPairCount = 30;
constexpr std::size_t kDenseActiveOrderCount = 120;
constexpr std::size_t kFeedbackStageCount =
    static_cast<std::size_t>(StrategyFeedbackStageForTest::kCount);
constexpr std::size_t kFeedbackProfileSegmentCount = kFeedbackStageCount + 1U;

[[nodiscard]] core::OrderPlaceRequest BenchmarkPlaceRequest(
    std::uint64_t local_order_id = 0, std::uint64_t parent_id = 0) noexcept {
  core::OrderPlaceRequest request{
      .local_order_id = local_order_id,
      .parent_id = parent_id,
      .price = 102.1,
      .quantity = 7.0,
      .symbol_id = kSymbolId,
      .gateway_route_id = 0,
      .exchange = Exchange::kGate,
      .side = OrderSide::kBuy,
      .order_type = OrderType::kLimit,
      .time_in_force = TimeInForce::kImmediateOrCancel,
      .price_decimal_places = 1,
      .quantity_decimal_places = 0,
      .reduce_only = false,
  };
  core::SetOrderSymbol(&request, "BTC_USDT");
  return request;
}

struct FeedbackStageCapture {
  std::array<std::uint64_t, kFeedbackStageCount> timestamps_ns{};
};

thread_local FeedbackStageCapture* feedback_stage_capture = nullptr;

void CaptureFeedbackStage(
    const detail::StrategyFeedbackStageRecordForTest& record) noexcept {
  if (feedback_stage_capture == nullptr) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(record.stage);
  if (index < feedback_stage_capture->timestamps_ns.size()) {
    feedback_stage_capture->timestamps_ns[index] =
        websocket::benchmarking::NowNs();
  }
}

class FeedbackStageObserverGuard {
 public:
  FeedbackStageObserverGuard() noexcept {
    detail::SetStrategyFeedbackStageObserverForTest(CaptureFeedbackStage);
  }

  ~FeedbackStageObserverGuard() noexcept {
    feedback_stage_capture = nullptr;
    detail::SetStrategyFeedbackStageObserverForTest(nullptr);
  }

  FeedbackStageObserverGuard(const FeedbackStageObserverGuard&) = delete;
  FeedbackStageObserverGuard& operator=(const FeedbackStageObserverGuard&) =
      delete;
};

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

  void Stop() noexcept {
    running = false;
  }

  [[nodiscard]] bool Ready() const noexcept {
    return true;
  }

  [[nodiscard]] bool Running() const noexcept {
    return running;
  }

  SendResult PlaceOrder(const core::OrderPlaceRequest& request) noexcept {
    if (state != nullptr) {
      ++state->place_calls;
      state->last_place_local_order_id = request.local_order_id;
    }
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk};
  }

  SendResult CancelOrder(const core::OrderCancelRequest&) noexcept {
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk};
  }

  SharedOrderSessionState* state{};
  bool running{false};
};

using Runtime = core::TradingRuntime<Strategy, BenchmarkOrderSession,
                                     market_data::RealtimeDataReader<>,
                                     core::TradingRuntimeDiagnostics>;
using BenchmarkOrderManager = core::OrderManager<BenchmarkOrderSession>;
using BenchmarkStrategyContext = core::StrategyContext<BenchmarkOrderSession>;

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

[[nodiscard]] Config BenchmarkConfig(Exchange lag_exchange,
                                     std::size_t pair_count) {
  Config config = BenchmarkConfig(lag_exchange);
  const PairConfig pair_template = config.pairs.front();
  config.pairs.reserve(pair_count);
  for (std::size_t index = 1; index < pair_count; ++index) {
    PairConfig pair = pair_template;
    pair.symbol_id += static_cast<std::int32_t>(index);
    pair.symbol = fmt::format("PAIR_{}", index);
    pair.lag_instrument.symbol_id = pair.symbol_id;
    pair.lag_instrument.exchange_symbol = fmt::format("PAIR_{}_LAG", index);
    config.pairs.push_back(std::move(pair));
  }
  return config;
}

[[nodiscard]] BookTicker Ticker(Exchange exchange, std::int64_t local_ns,
                                double bid_price, double ask_price) noexcept {
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
  const auto base_ns = benchmarking::RealtimeNowNs();
  runtime.HandleBookTickerForTest(
      Ticker(lag_exchange, base_ns, 101.57, 102.02));
  runtime.HandleBookTickerForTest(
      Ticker(Exchange::kBinance, base_ns + 1'000, 100.0, 101.0));
  runtime.HandleBookTickerForTest(
      Ticker(Exchange::kBinance, base_ns + 2'000, 112.0, 113.0));
  return state.place_calls == 1 && state.last_place_local_order_id != 0;
}

[[nodiscard]] bool SeedPendingOpenOrder(Strategy& strategy,
                                        BenchmarkStrategyContext& context,
                                        SharedOrderSessionState& state,
                                        Exchange lag_exchange) {
  const auto base_ns = benchmarking::RealtimeNowNs();
  strategy.OnBookTicker(Ticker(lag_exchange, base_ns, 101.57, 102.02), context);
  strategy.OnBookTicker(
      Ticker(Exchange::kBinance, base_ns + 1'000, 100.0, 101.0), context);
  strategy.OnBookTicker(
      Ticker(Exchange::kBinance, base_ns + 2'000, 112.0, 113.0), context);
  return state.place_calls == 1 && state.last_place_local_order_id != 0;
}

[[nodiscard]] OrderFeedbackEvent TerminalFillEvent(
    std::uint64_t local_order_id) noexcept {
  return {
      .kind = OrderFeedbackKind::kFilled,
      .local_order_id = local_order_id,
      .exchange_order_id = local_order_id + 1000,
      .cumulative_filled_quantity = 7.0,
      .left_quantity = 0.0,
      .cancelled_quantity = 0.0,
      .fill_price = 102.1,
      .role = OrderRole::kTaker,
      .finish_reason = OrderFinishReason::kUnknown,
      .reject_reason = OrderRejectReason::kUnknown,
      .continuity_scope = OrderFeedbackContinuityScope::kLane,
      .continuity_reason = OrderFeedbackContinuityReason::kUnknown,
      .continuity_sequence = 0,
      .exchange_update_ns = 200'000,
      .local_receive_ns = kFeedbackLocalReceiveNs,
  };
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
        RuntimeConfig(),
        [&session_state] { return BenchmarkOrderSession{&session_state}; },
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

    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const gate::OrderFeedbackParseResult parsed =
        gate::ParseGateOrderFeedbackMessage(
            payload, kFeedbackLocalReceiveNs, parser_stats,
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
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
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

    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
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
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "feedback_events", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

void RunLeadLagRuntimeTerminalFillLatency(benchmark::State& state,
                                          Exchange lag_exchange) {
  benchmarking::EnsureLoggingStarted();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    state.PauseTiming();
    SharedOrderSessionState session_state;
    auto runtime_result = Runtime::CreateForTest(
        RuntimeConfig(),
        [&session_state] { return BenchmarkOrderSession{&session_state}; },
        BenchmarkConfig(lag_exchange));
    if (!runtime_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(runtime_result.error.c_str());
      return;
    }

    Runtime& runtime = *runtime_result.value;
    if (!SeedPendingOpenOrder(runtime, session_state, lag_exchange)) {
      state.ResumeTiming();
      state.SkipWithError("failed to seed pending lead lag order");
      return;
    }

    const OrderFeedbackEvent event =
        TerminalFillEvent(session_state.last_place_local_order_id);

    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    runtime.HandleOrderFeedbackForTest(event);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.PauseTiming();

    if (runtime.order_manager().order_count() != 0) {
      state.ResumeTiming();
      state.SkipWithError("terminal feedback did not retire the order");
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "feedback_events", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

void BM_LeadLagRuntimeGateTerminalFillLatency(benchmark::State& state) {
  RunLeadLagRuntimeTerminalFillLatency(state, Exchange::kGate);
}

void BM_LeadLagRuntimeBitgetTerminalFillLatency(benchmark::State& state) {
  RunLeadLagRuntimeTerminalFillLatency(state, Exchange::kBitget);
}

void BM_LeadLagOrderManagerTerminalFillLatency(benchmark::State& state) {
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    state.PauseTiming();
    SharedOrderSessionState session_state;
    BenchmarkOrderSession session(&session_state);
    BenchmarkOrderManager order_manager(session, kOrderCapacity, kStrategyId);
    const core::OrderPlaceResult placed =
        order_manager.PlaceLimitOrder(BenchmarkPlaceRequest());
    if (placed.status != core::OrderPlaceStatus::kOk) {
      state.ResumeTiming();
      state.SkipWithError("failed to seed order manager");
      return;
    }
    const OrderFeedbackEvent event = TerminalFillEvent(placed.local_order_id);

    state.ResumeTiming();
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    order_manager.OnOrderFeedback(event);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.PauseTiming();

    const core::StrategyOrder* order =
        order_manager.FindOrder(placed.local_order_id);
    if (order == nullptr || !order->is_finished ||
        order->status != core::OrderStatus::kFilled) {
      state.ResumeTiming();
      state.SkipWithError("order manager did not apply terminal feedback");
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "feedback_events", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

void RunLeadLagStrategyTerminalFillLatency(benchmark::State& state,
                                           Exchange lag_exchange) {
  benchmarking::EnsureLoggingStarted();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    state.PauseTiming();
    SharedOrderSessionState session_state;
    BenchmarkOrderSession session(&session_state);
    BenchmarkOrderManager order_manager(session, kOrderCapacity, kStrategyId);
    BenchmarkStrategyContext context(order_manager);
    auto strategy = std::make_unique<Strategy>(BenchmarkConfig(lag_exchange));
    if (!SeedPendingOpenOrder(*strategy, context, session_state,
                              lag_exchange)) {
      state.ResumeTiming();
      state.SkipWithError("failed to seed pending lead lag order");
      return;
    }
    const OrderFeedbackEvent event =
        TerminalFillEvent(session_state.last_place_local_order_id);
    order_manager.OnOrderFeedback(event);

    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    strategy->OnOrderFeedback(event, context);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.PauseTiming();

    if (order_manager.order_count() != 0) {
      state.ResumeTiming();
      state.SkipWithError("strategy did not retire the terminal order");
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "feedback_events", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

void SetFeedbackProfileSegmentCounters(benchmark::State& state,
                                       std::string_view name,
                                       std::vector<std::uint64_t> samples_ns) {
  if (samples_ns.empty()) {
    return;
  }
  state.counters[fmt::format("{}_p50_ns", name)] = static_cast<double>(
      websocket::benchmarking::SelectQuantile(samples_ns, 0.50));
  state.counters[fmt::format("{}_p99_ns", name)] = static_cast<double>(
      websocket::benchmarking::SelectQuantile(samples_ns, 0.99));
  state.counters[fmt::format("{}_p999_ns", name)] = static_cast<double>(
      websocket::benchmarking::SelectQuantile(samples_ns, 0.999));
}

void BM_LeadLagStrategyGateTerminalFillStageProfile(benchmark::State& state) {
  static constexpr std::array<std::string_view, kFeedbackProfileSegmentCount>
      kSegmentNames{
          "context",   "feedback_log", "finished_lookup", "position_fields",
          "execution", "finished_log", "retire",          "observer_tail",
      };

  benchmarking::EnsureLoggingStarted();
  FeedbackStageObserverGuard observer_guard;
  std::array<std::vector<std::uint64_t>, kFeedbackProfileSegmentCount>
      segment_samples_ns;
  std::vector<std::uint64_t> total_samples_ns;
  for (std::vector<std::uint64_t>& samples : segment_samples_ns) {
    samples.reserve(kLatencyIterations);
  }
  total_samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    state.PauseTiming();
    SharedOrderSessionState session_state;
    BenchmarkOrderSession session(&session_state);
    BenchmarkOrderManager order_manager(session, kOrderCapacity, kStrategyId);
    BenchmarkStrategyContext context(order_manager);
    auto strategy =
        std::make_unique<Strategy>(BenchmarkConfig(Exchange::kGate));
    if (!SeedPendingOpenOrder(*strategy, context, session_state,
                              Exchange::kGate)) {
      state.ResumeTiming();
      state.SkipWithError("failed to seed pending lead lag order");
      return;
    }
    const OrderFeedbackEvent event =
        TerminalFillEvent(session_state.last_place_local_order_id);
    order_manager.OnOrderFeedback(event);
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    FeedbackStageCapture capture;
    feedback_stage_capture = &capture;
    state.ResumeTiming();

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    strategy->OnOrderFeedback(event, context);
    const std::uint64_t end_ns = websocket::benchmarking::NowNs();
    feedback_stage_capture = nullptr;
    state.PauseTiming();

    bool capture_complete = true;
    std::uint64_t previous_ns = start_ns;
    for (std::size_t index = 0; index < kFeedbackStageCount; ++index) {
      const std::uint64_t timestamp_ns = capture.timestamps_ns[index];
      if (timestamp_ns < previous_ns) {
        capture_complete = false;
        break;
      }
      segment_samples_ns[index].push_back(timestamp_ns - previous_ns);
      previous_ns = timestamp_ns;
    }
    if (!capture_complete || end_ns < previous_ns ||
        order_manager.order_count() != 0) {
      state.ResumeTiming();
      state.SkipWithError("feedback stage capture failed");
      return;
    }
    segment_samples_ns.back().push_back(end_ns - previous_ns);
    const std::uint64_t elapsed_ns = end_ns - start_ns;
    total_samples_ns.push_back(elapsed_ns);
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(total_samples_ns), "feedback_events",
      state.iterations());
  for (std::size_t index = 0; index < segment_samples_ns.size(); ++index) {
    SetFeedbackProfileSegmentCounters(state, kSegmentNames[index],
                                      std::move(segment_samples_ns[index]));
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_LeadLagStrategyGateTerminalFillLatency(benchmark::State& state) {
  RunLeadLagStrategyTerminalFillLatency(state, Exchange::kGate);
}

void BM_LeadLagStrategyBitgetTerminalFillLatency(benchmark::State& state) {
  RunLeadLagStrategyTerminalFillLatency(state, Exchange::kBitget);
}

[[nodiscard]] core::StrategyOrder TerminalFilledOrder() noexcept {
  return {
      .place_request = BenchmarkPlaceRequest(1, 2),
      .exchange_order_id = 1001,
      .status = core::OrderStatus::kFilled,
      .cumulative_filled_quantity = 7.0,
      .cumulative_filled_value = 714.7,
      .last_fill_price = 102.1,
      .request_send_local_ns = 100,
      .ack_local_receive_ns = 150,
      .ack_exchange_ns = 140,
      .finish_exchange_ns = 200'000,
      .exchange_update_ns = 200'000,
      .role = OrderRole::kTaker,
      .is_finished = true,
  };
}

void BM_LeadLagLogOrderFeedbackLatency(benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  const core::StrategyOrder order = TerminalFilledOrder();
  const OrderFeedbackEvent event =
      TerminalFillEvent(order.place_request.local_order_id);
  const SignalTiming market_timing{
      .lead_exchange_ns = 180'000,
      .lead_book_ticker_id = 11,
      .lag_exchange_ns = 190'000,
      .lag_book_ticker_id = 12,
  };
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    state.PauseTiming();
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    detail::LogStrategyOrderFeedback(event, &order, market_timing);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    state.PauseTiming();
    samples_ns.push_back(elapsed_ns);
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "log_records", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

void BM_LeadLagLogOrderFinishedLatency(benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  const core::StrategyOrder order = TerminalFilledOrder();
  const detail::StrategyOrderPositionLogFields position{
      .position_id = 7,
      .position_direction = PositionDirection::kLong,
      .order_role = "entry",
      .entry_local_order_id = order.place_request.local_order_id,
  };
  const SignalTiming market_timing{
      .lead_exchange_ns = 180'000,
      .lag_exchange_ns = 190'000,
  };
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    state.PauseTiming();
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    detail::LogStrategyOrderFinished(order, position, 1, market_timing);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    state.PauseTiming();
    samples_ns.push_back(elapsed_ns);
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "log_records", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

void BM_LeadLagLogTerminalFeedbackPairLatency(benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  const core::StrategyOrder order = TerminalFilledOrder();
  const OrderFeedbackEvent event =
      TerminalFillEvent(order.place_request.local_order_id);
  const detail::StrategyOrderPositionLogFields position{
      .position_id = 7,
      .position_direction = PositionDirection::kLong,
      .order_role = "entry",
      .entry_local_order_id = order.place_request.local_order_id,
  };
  const SignalTiming market_timing{
      .lead_exchange_ns = 180'000,
      .lead_book_ticker_id = 11,
      .lag_exchange_ns = 190'000,
      .lag_book_ticker_id = 12,
  };
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    state.PauseTiming();
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    detail::LogStrategyOrderFeedback(event, &order, market_timing);
    detail::LogStrategyOrderFinished(order, position, 1, market_timing);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    state.PauseTiming();
    samples_ns.push_back(elapsed_ns);
    if (nova::kLogManager.logger() != nullptr) {
      nova::kLogManager.logger()->flush_log();
    }
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "log_records", state.iterations() * 2U);
  state.SetItemsProcessed(state.iterations());
}

void BM_LeadLagExecutionApplyTerminalOrderLatency(benchmark::State& state) {
  const InstrumentMetadata instrument =
      BenchmarkConfig(Exchange::kGate).pairs.front().lag_instrument;
  const core::StrategyOrder order = TerminalFilledOrder();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    state.PauseTiming();
    ExecutionState execution;
    execution.Init(1);
    if (execution.StartOpenOrder(order.place_request.local_order_id) ==
        nullptr) {
      state.ResumeTiming();
      state.SkipWithError("failed to seed execution state");
      return;
    }
    state.ResumeTiming();

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const ExecutionApplyResult result =
        execution.ApplyTerminalOrder(order, instrument);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.PauseTiming();

    if (result != ExecutionApplyResult::kAppliedHold ||
        execution.active_group_count() != 1) {
      state.ResumeTiming();
      state.SkipWithError("execution state did not apply terminal order");
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "terminal_orders", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

void RunOrderPriceTextEraseLatency(benchmark::State& state,
                                   std::size_t target_index,
                                   std::size_t dense_active_count) {
  auto strategy = std::make_unique<Strategy>(
      BenchmarkConfig(Exchange::kGate, kActualPairCount));
  if (target_index >= strategy->OrderRiskSlotCountForTest()) {
    state.SkipWithError("order risk target slot is unavailable");
    return;
  }

  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);
  for (auto _ : state) {
    state.PauseTiming();
    const std::uint64_t local_order_id =
        strategy->PrepareOrderRiskSlotEraseForTest(target_index,
                                                   dense_active_count);
    state.ResumeTiming();

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    strategy->EraseOrderRiskSlotForTest(local_order_id);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.PauseTiming();

    if (local_order_id == 0 ||
        strategy->OrderRiskSlotActiveForTest(target_index)) {
      state.ResumeTiming();
      state.SkipWithError("order risk slot was not erased");
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(
      state, std::move(samples_ns), "erased_slots", state.iterations());
  state.SetItemsProcessed(state.iterations());
}

void BM_LeadLagOrderPriceTextEraseFirstSlotLatency(benchmark::State& state) {
  RunOrderPriceTextEraseLatency(state, 0, 1);
}

void BM_LeadLagOrderPriceTextEraseDense120Latency(benchmark::State& state) {
  RunOrderPriceTextEraseLatency(state, kDenseActiveOrderCount - 1,
                                kDenseActiveOrderCount);
}

void BM_LeadLagOrderPriceTextEraseSparseLastSlotLatency(
    benchmark::State& state) {
  const std::size_t slot_count =
      kActualPairCount * kMaxExecutionGroupPendingOrders;
  RunOrderPriceTextEraseLatency(state, slot_count - 1, 0);
}

BENCHMARK(BM_LeadLagFeedbackParserShmToRuntimeTerminalFillLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagBitgetFeedbackParserShmToRuntimeTerminalFillLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagRuntimeGateTerminalFillLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagRuntimeBitgetTerminalFillLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagOrderManagerTerminalFillLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagStrategyGateTerminalFillLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagStrategyBitgetTerminalFillLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagStrategyGateTerminalFillStageProfile)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagLogOrderFeedbackLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagLogOrderFinishedLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagLogTerminalFeedbackPairLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagExecutionApplyTerminalOrderLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagOrderPriceTextEraseFirstSlotLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagOrderPriceTextEraseDense120Latency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagOrderPriceTextEraseSparseLastSlotLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila::strategy::leadlag
