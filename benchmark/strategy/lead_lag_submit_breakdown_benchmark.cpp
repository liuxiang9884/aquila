#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include "benchmark/strategy/lead_lag_benchmark_support.h"
#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/common/types.h"
#include "core/config/instrument_catalog.h"
#include "core/config/strategy_config.h"
#include "core/market_data/types.h"
#include "core/trading/order_decimal.h"
#include "core/trading/order_gateway_client.h"
#include "core/trading/order_manager.h"
#include "core/trading/order_types.h"
#include "core/trading/trading_runtime.h"
#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "exchange/bitget/trading/order_gateway_worker.h"
#include "exchange/bitget/trading/order_session.h"
#include "exchange/gate/trading/order_gateway_worker.h"
#include "exchange/gate/trading/order_session.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/strategy.h"
#include "strategy/lead_lag/strategy_test_hooks.h"
#include <simdjson.h>

namespace aquila::strategy::leadlag {
namespace {

constexpr std::uint8_t kStrategyId = 4;
constexpr std::int32_t kSymbolId = 67;
constexpr std::int32_t kWarmupSymbolId = 467;
constexpr std::size_t kOrderCapacity = 64;
constexpr std::uint16_t kFanout = 4;
constexpr std::uint16_t kSingleRouteFanout = 1;
constexpr std::size_t kLatencyIterations = 4096;
constexpr std::size_t kSubmitBreakdownIterations = 1024;
constexpr std::size_t kColdSubmitBreakdownIterations = 1024;
constexpr std::size_t kGatewayQueueCapacity = kLatencyIterations + 128;
constexpr std::string_view kSymbol = "BAS_USDT";
constexpr std::string_view kActualLiveLeadLagConfigPath =
    "config/strategies/"
    "lead_lag_30symbols_fusion_2bps_2bps_5bps_lag200_order_gateway_20260701."
    "toml";
constexpr std::string_view kActualLiveInstrumentCatalogPath =
    "config/instruments/usdt_future_universe.csv";
constexpr std::string_view kBitgetCombined46LeadLagConfigPath =
    "config/strategies/"
    "lead_lag_bitget_combined_46symbols_highspeed_fanout4_20260718.toml";
constexpr std::string_view kGateLoginSuccess = R"json({
  "request_id": "72057594037927937",
  "ack": false,
  "header": {
    "status": "200",
    "channel": "futures.login",
    "event": "api"
  },
  "data": {
    "result": {
      "uid": "1"
    }
  }
})json";
constexpr std::string_view kBitgetLoginSuccess =
    R"({"event":"login","code":"0","msg":""})";

bitget::ClientOidRunNamespace BitgetRunNamespace() {
  return bitget::ClientOidRunNamespace::Parse("0123456789AB").value();
}

struct LiveSymbolSpec {
  std::string_view symbol;
  std::int32_t symbol_id{0};
};

constexpr std::array<LiveSymbolSpec, 30> kLiveSymbolSpecs{{
    {"BTC_USDT", 93},     {"SOL_USDT", 384},  {"DOGE_USDT", 137},
    {"XRP_USDT", 472},    {"HYPE_USDT", 210}, {"TAC_USDT", 411},
    {"ZEC_USDT", 480},    {"ORDI_USDT", 316}, {"AIGENSYN_USDT", 15},
    {"WLD_USDT", 460},    {"SLX_USDT", 381},  {"UB_USDT", 438},
    {"VELVET_USDT", 449}, {"BTW_USDT", 95},   {"RAVE_USDT", 347},
    {"SUI_USDT", 404},    {"AVAX_USDT", 51},  {"ENA_USDT", 152},
    {"BAS_USDT", 67},     {"H_USDT", 211},    {"LINK_USDT", 253},
    {"RE_USDT", 353},     {"IN_USDT", 222},   {"AAVE_USDT", 4},
    {"XLM_USDT", 467},    {"GWEI_USDT", 196}, {"NEAR_USDT", 293},
    {"AGLD_USDT", 12},    {"UNI_USDT", 440},  {"BCH_USDT", 70},
}};

struct E2ETransportStats {
  std::uint64_t invalid_writes{0};
  std::uint64_t write_calls{0};
  std::uint64_t bytes_sent{0};
  std::int64_t last_write_ns{0};
};

thread_local std::int64_t e2e_order_session_place_entry_ns = 0;

class E2ECountingOrderTransport {
 public:
  static constexpr bool kUsesTls = false;

  bool Init() noexcept {
    wants_read_ = false;
    wants_write_ = false;
    return true;
  }

  bool OpenAndConnect(const websocket::ConnectionConfig&) noexcept {
    return Init();
  }

  bool FinishHandshake() noexcept {
    return true;
  }

  ssize_t ReadSome(std::span<std::byte>) noexcept {
    wants_read_ = true;
    wants_write_ = false;
    errno = EAGAIN;
    return -1;
  }

  [[nodiscard]] std::size_t PendingReadableBytes() const noexcept {
    return 0;
  }

  ssize_t WriteSome(std::span<const std::byte> buffer) noexcept {
    wants_read_ = false;
    wants_write_ = false;
    if (buffer.empty()) {
      ++stats_.invalid_writes;
      errno = EINVAL;
      return -1;
    }
    ++stats_.write_calls;
    stats_.bytes_sent += static_cast<std::uint64_t>(buffer.size());
    stats_.last_write_ns = benchmarking::RealtimeNowNs();
    return static_cast<ssize_t>(buffer.size());
  }

  [[nodiscard]] bool WantsRead() const noexcept {
    return wants_read_;
  }

  [[nodiscard]] bool WantsWrite() const noexcept {
    return wants_write_;
  }

  [[nodiscard]] int NativeFd() const noexcept {
    return -1;
  }

  void Close() noexcept {
    wants_read_ = false;
    wants_write_ = false;
  }

  static void ResetStats() noexcept {
    stats_ = {};
  }

  [[nodiscard]] static E2ETransportStats stats() noexcept {
    return stats_;
  }

 private:
  bool wants_read_{false};
  bool wants_write_{false};

  inline static E2ETransportStats stats_{};
};

struct E2EOrderWebSocketPolicy : websocket::DefaultWebSocketOptions {
  using TransportSocket = E2ECountingOrderTransport;
};

[[nodiscard]] websocket::ConnectionConfig GateE2EConnectionConfig() {
  websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.port = "80";
  config.target = "/v4/ws/usdt";
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 4096;
  return config;
}

[[nodiscard]] websocket::ConnectionConfig BitgetE2EConnectionConfig() {
  websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.port = "80";
  config.target = "/v3/ws/private";
  config.enable_tls = false;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 4096;
  return config;
}

template <std::size_t N>
[[nodiscard]] websocket::MessageView PaddedTextView(
    std::string_view payload, std::array<char, N>* storage) noexcept {
  static_assert(N >= simdjson::SIMDJSON_PADDING);
  if (payload.size() > N - simdjson::SIMDJSON_PADDING) {
    return {};
  }
  std::memcpy(storage->data(), payload.data(), payload.size());
  return {
      .kind = websocket::PayloadKind::kText,
      .payload = std::as_bytes(std::span(storage->data(), payload.size())),
      .sequence = 1,
      .fin = true,
      .readable_tail_bytes = simdjson::SIMDJSON_PADDING,
  };
}

struct GateE2EResponseHandler {
  void OnOrderResponse(const gate::OrderResponse&) noexcept {}
};

class GateE2EOrderSession {
 public:
  using Session =
      gate::OrderSession<GateE2EResponseHandler, E2EOrderWebSocketPolicy>;

  GateE2EOrderSession()
      : handler_(std::make_unique<GateE2EResponseHandler>()),
        session_(std::make_unique<Session>(
            GateE2EConnectionConfig(),
            gate::LoginCredentials{.api_key = "key", .api_secret = "secret"},
            *handler_, kOrderCapacity)) {
    session_->OnConnectionPhase(websocket::ConnectionPhase::kActive);
    std::array<char, 512 + simdjson::SIMDJSON_PADDING> payload{};
    ready_ = session_->Handle(PaddedTextView(kGateLoginSuccess, &payload)) ==
                 websocket::DeliveryResult::kAccepted &&
             session_->login_ready();
  }

  GateE2EOrderSession(GateE2EOrderSession&&) noexcept = default;
  GateE2EOrderSession& operator=(GateE2EOrderSession&&) noexcept = default;
  GateE2EOrderSession(const GateE2EOrderSession&) = delete;
  GateE2EOrderSession& operator=(const GateE2EOrderSession&) = delete;

  [[nodiscard]] bool Ready() const noexcept {
    return ready_;
  }

  [[nodiscard]] bool WarmPlaceOrder() noexcept {
    core::OrderPlaceRequest request{
        .local_order_id = 9'000'001,
        .price = 0.052045,
        .quantity = 192.0,
        .exchange = Exchange::kGate,
        .side = OrderSide::kBuy,
        .order_type = OrderType::kLimit,
        .time_in_force = TimeInForce::kImmediateOrCancel,
        .price_decimal_places = 6,
        .quantity_decimal_places = 0,
    };
    core::SetOrderSymbol(&request, "BAS_USDT");
    std::array<char, gate::kPlaceOrderRequestBufferSize> buffer;
    for (std::uint64_t sequence = 1; sequence <= 64; ++sequence) {
      if (gate::EncodePlaceOrderRequest(request, 0, sequence, false, buffer)
              .status != gate::OrderEncodeStatus::kOk) {
        return false;
      }
    }
    return session_->PlaceOrder(request).status == gate::OrderSendStatus::kOk;
  }

  gate::OrderSendResult PlaceOrder(
      const core::OrderPlaceRequest& request) noexcept {
    e2e_order_session_place_entry_ns = benchmarking::RealtimeNowNs();
    return session_->PlaceOrder(request);
  }

  gate::OrderSendResult CancelOrder(
      const core::OrderCancelRequest& request) noexcept {
    return session_->CancelOrder(request);
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    session_->CacheExchangeOrderId(local_order_id, exchange_order_id);
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    session_->ForgetExchangeOrderId(local_order_id);
  }

 private:
  std::unique_ptr<GateE2EResponseHandler> handler_;
  std::unique_ptr<Session> session_;
  bool ready_{false};
};

struct BitgetE2EResponseHandler {
  void OnOrderResponse(const bitget::OrderResponse&) noexcept {}
};

class BitgetE2EOrderSession {
 public:
  using Session =
      bitget::OrderSession<BitgetE2EResponseHandler, E2EOrderWebSocketPolicy>;

  BitgetE2EOrderSession()
      : handler_(std::make_unique<BitgetE2EResponseHandler>()),
        session_(std::make_unique<Session>(
            BitgetE2EConnectionConfig(),
            bitget::LoginCredentials{.api_key = "key",
                                     .api_secret = "secret",
                                     .passphrase = "phrase"},
            BitgetRunNamespace(), *handler_, kOrderCapacity, kOrderCapacity)) {
    session_->OnConnectionPhase(websocket::ConnectionPhase::kActive);
    std::array<char, 128 + simdjson::SIMDJSON_PADDING> payload{};
    ready_ = session_->Handle(PaddedTextView(kBitgetLoginSuccess, &payload)) ==
                 websocket::DeliveryResult::kAccepted &&
             session_->Ready();
  }

  BitgetE2EOrderSession(BitgetE2EOrderSession&&) noexcept = default;
  BitgetE2EOrderSession& operator=(BitgetE2EOrderSession&&) noexcept = default;
  BitgetE2EOrderSession(const BitgetE2EOrderSession&) = delete;
  BitgetE2EOrderSession& operator=(const BitgetE2EOrderSession&) = delete;

  [[nodiscard]] bool Ready() const noexcept {
    return ready_;
  }

  [[nodiscard]] bool WarmPlaceOrder() noexcept {
    core::OrderPlaceRequest request{
        .local_order_id = 9'000'002,
        .price = 0.052045,
        .quantity = 192.0,
        .exchange = Exchange::kBitget,
        .side = OrderSide::kBuy,
        .order_type = OrderType::kLimit,
        .time_in_force = TimeInForce::kImmediateOrCancel,
        .price_decimal_places = 6,
        .quantity_decimal_places = 0,
    };
    core::SetOrderSymbol(&request, "BASUSDT");
    std::array<char, bitget::kPlaceOrderRequestBufferSize> buffer;
    for (std::uint64_t sequence = 1; sequence <= 64; ++sequence) {
      if (bitget::EncodePlaceOrderRequest(request, BitgetRunNamespace(),
                                          sequence, buffer)
              .status != bitget::OrderEncodeStatus::kOk) {
        return false;
      }
    }
    return session_->PlaceOrder(request).status == bitget::OrderSendStatus::kOk;
  }

  bitget::OrderSendResult PlaceOrder(
      const core::OrderPlaceRequest& request) noexcept {
    e2e_order_session_place_entry_ns = benchmarking::RealtimeNowNs();
    return session_->PlaceOrder(request);
  }

  bitget::OrderSendResult CancelOrder(
      const core::OrderCancelRequest& request) noexcept {
    return session_->CancelOrder(request);
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    session_->CacheExchangeOrderId(local_order_id, exchange_order_id);
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    session_->ForgetExchangeOrderId(local_order_id);
  }

 private:
  std::unique_ptr<BitgetE2EResponseHandler> handler_;
  std::unique_ptr<Session> session_;
  bool ready_{false};
};

class E2EShmCleanup {
 public:
  explicit E2EShmCleanup(std::string shm_name)
      : shm_name_(std::move(shm_name)) {}

  ~E2EShmCleanup() {
    ::shm_unlink(shm_name_.c_str());
  }

  E2EShmCleanup(const E2EShmCleanup&) = delete;
  E2EShmCleanup& operator=(const E2EShmCleanup&) = delete;

 private:
  std::string shm_name_;
};

template <typename OrderSessionT>
struct E2EExchangeTraits;

template <>
struct E2EExchangeTraits<GateE2EOrderSession> {
  using Publisher = gate::OrderGatewayWorkerPublisher;
  using Worker = gate::OrderGatewayCommandWorker<GateE2EOrderSession>;
};

template <>
struct E2EExchangeTraits<BitgetE2EOrderSession> {
  using Publisher = bitget::OrderGatewayWorkerPublisher;
  using Worker = bitget::OrderGatewayCommandWorker<BitgetE2EOrderSession>;
};

void EnsureE2ELoggingInitialized() {
  static const bool initialized = [] {
    nova::LogConfig config;
    config.set_console_sink_name("");
    config.set_file_sink_name("/dev/null");
    config.set_log_level("critical");
    nova::InitializeLogging(config);
    return true;
  }();
  (void)initialized;
}

[[nodiscard]] std::uint64_t DeltaNs(std::int64_t start_ns,
                                    std::int64_t end_ns) noexcept {
  if (end_ns <= start_ns) {
    return 0;
  }
  return static_cast<std::uint64_t>(end_ns - start_ns);
}

[[nodiscard]] std::uint64_t Quantile(std::span<const std::uint64_t> samples,
                                     double quantile) {
  if (samples.empty()) {
    return 0;
  }
  std::vector<std::uint64_t> copy(samples.begin(), samples.end());
  return websocket::benchmarking::SelectQuantile(copy, quantile);
}

void SetPrefixedLatencyCounters(benchmark::State& state,
                                std::string_view prefix,
                                std::span<const std::uint64_t> samples) {
  if (samples.empty()) {
    return;
  }
  const std::string name{prefix};
  state.counters[name + "_p50_ns"] =
      static_cast<double>(Quantile(samples, 0.50));
  state.counters[name + "_p95_ns"] =
      static_cast<double>(Quantile(samples, 0.95));
  state.counters[name + "_p99_ns"] =
      static_cast<double>(Quantile(samples, 0.99));
  state.counters[name + "_max_ns"] =
      static_cast<double>(*std::max_element(samples.begin(), samples.end()));
}

template <typename Func>
void RunManualLatencyBenchmark(benchmark::State& state, Func&& func,
                               std::string_view detail_name = {},
                               std::uint64_t detail_value = 0) {
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    func();
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              detail_name, detail_value);
}

[[nodiscard]] SignalTiming SyntheticTiming() noexcept {
  return SignalTiming{
      .trigger_exchange_ns = 1'782'902'031'601'000'000LL,
      .trigger_local_ns = 1'782'902'031'601'583'635LL,
      .on_book_ticker_entry_ns = 1'782'902'031'601'584'371LL,
      .signal_decision_ns = 1'782'902'031'601'585'317LL,
      .lead_exchange_ns = 1'782'902'031'601'000'000LL,
      .lead_local_ns = 1'782'902'031'601'583'635LL,
      .lead_book_ticker_id = 10'939'018'256'302LL,
      .lead_freshness_ns = 585'317,
      .lag_exchange_ns = 1'782'902'031'600'849'000LL,
      .lag_local_ns = 1'782'902'031'600'989'544LL,
      .lag_book_ticker_id = 1'192'411'491,
      .lag_freshness_ns = 736'317,
      .max_lead_freshness_ns = 5'000'000,
      .max_lag_freshness_ns = 200'000'000,
  };
}

[[nodiscard]] detail::StrategyOrderPositionLogFields
SyntheticPositionLog() noexcept {
  return detail::StrategyOrderPositionLogFields{
      .position_id = 1,
      .position_event = "kEntrySubmit",
      .position_direction = PositionDirection::kLong,
      .order_role = "entry",
      .entry_local_order_id = 432345564227567625ULL,
  };
}

[[nodiscard]] core::OrderPlaceRequest SyntheticOrderRequest(
    std::uint64_t local_order_id = 0, std::uint64_t group_id = 1,
    std::uint16_t route_id = 0) noexcept {
  core::OrderPlaceRequest request{
      .local_order_id = local_order_id,
      .group_id = group_id,
      .price = 0.052045,
      .quantity = 1.0,
      .symbol_id = kSymbolId,
      .gateway_route_id = route_id,
      .exchange = Exchange::kGate,
      .side = OrderSide::kBuy,
      .order_type = OrderType::kLimit,
      .time_in_force = TimeInForce::kImmediateOrCancel,
      .price_decimal_places = 6,
      .quantity_decimal_places = 0,
      .reduce_only = false,
  };
  core::SetOrderSymbol(&request, kSymbol);
  return request;
}

[[nodiscard]] std::array<core::OrderPlaceRequest, kFanout>
SyntheticFanoutOrders(std::uint64_t first_local_order_id,
                      std::uint64_t group_id) noexcept {
  std::array<core::OrderPlaceRequest, kFanout> orders{};
  for (std::uint16_t route = 0; route < kFanout; ++route) {
    orders[route] =
        SyntheticOrderRequest(first_local_order_id + route, group_id, route);
  }
  return orders;
}

struct SubmitTrace {
  std::int64_t signal_decision_ns{0};
  std::int64_t signal_trigger_observer_ns{0};
  std::int64_t handle_end_ns{0};
  std::array<std::int64_t,
             static_cast<std::size_t>(StrategySubmitStageForTest::kCount)>
      stage_ns{};
  std::array<std::array<std::int64_t, kFanout>,
             static_cast<std::size_t>(StrategySubmitStageForTest::kCount)>
      route_stage_ns{};
  std::array<std::int64_t, kFanout> place_enter_ns{};
  std::array<std::int64_t, kFanout> submitted_observer_ns{};
  std::uint32_t place_calls{0};
  std::uint32_t submitted_calls{0};

  void Reset() noexcept {
    signal_decision_ns = 0;
    signal_trigger_observer_ns = 0;
    handle_end_ns = 0;
    stage_ns.fill(0);
    for (auto& stages : route_stage_ns) {
      stages.fill(0);
    }
    place_enter_ns.fill(0);
    submitted_observer_ns.fill(0);
    place_calls = 0;
    submitted_calls = 0;
  }
};

thread_local SubmitTrace* active_submit_trace = nullptr;

[[nodiscard]] constexpr std::size_t StageIndex(
    StrategySubmitStageForTest stage) noexcept {
  return static_cast<std::size_t>(stage);
}

[[nodiscard]] std::int64_t StageTimeNs(
    const SubmitTrace& trace, StrategySubmitStageForTest stage) noexcept {
  return trace.stage_ns[StageIndex(stage)];
}

[[nodiscard]] std::int64_t RouteStageTimeNs(const SubmitTrace& trace,
                                            StrategySubmitStageForTest stage,
                                            std::uint16_t route_id) noexcept {
  if (route_id >= kFanout) {
    return 0;
  }
  return trace.route_stage_ns[StageIndex(stage)][route_id];
}

void PushDelta(std::vector<std::uint64_t>* samples, std::int64_t start_ns,
               std::int64_t end_ns) {
  samples->push_back(DeltaNs(start_ns, end_ns));
}

struct SubmitStageSamples {
  std::vector<std::uint64_t> decision_to_price;
  std::vector<std::uint64_t> price_to_signal_decision_log;
  std::vector<std::uint64_t> signal_decision_log_to_freshness;
  std::vector<std::uint64_t> freshness_to_quantity;
  std::vector<std::uint64_t> quantity_to_routes_refreshed;
  std::vector<std::uint64_t> routes_refreshed_to_routes_selected;
  std::vector<std::uint64_t> routes_selected_to_risk;
  std::vector<std::uint64_t> risk_to_group;
  std::vector<std::uint64_t> group_to_route0_acquire_begin;
  std::vector<std::uint64_t> route0_acquire_text;
  std::vector<std::uint64_t> route0_acquire_done_to_place_begin;
  std::vector<std::uint64_t> route0_place_order;
  std::vector<std::uint64_t> route0_after_place_to_submit_result;
  std::vector<std::uint64_t> route0_place_to_route3_place;
  std::vector<std::uint64_t> route3_submit_result_to_done;

  void Reserve(std::size_t size) {
    decision_to_price.reserve(size);
    price_to_signal_decision_log.reserve(size);
    signal_decision_log_to_freshness.reserve(size);
    freshness_to_quantity.reserve(size);
    quantity_to_routes_refreshed.reserve(size);
    routes_refreshed_to_routes_selected.reserve(size);
    routes_selected_to_risk.reserve(size);
    risk_to_group.reserve(size);
    group_to_route0_acquire_begin.reserve(size);
    route0_acquire_text.reserve(size);
    route0_acquire_done_to_place_begin.reserve(size);
    route0_place_order.reserve(size);
    route0_after_place_to_submit_result.reserve(size);
    route0_place_to_route3_place.reserve(size);
    route3_submit_result_to_done.reserve(size);
  }

  void Push(const SubmitTrace& trace, std::uint16_t route_count = kFanout) {
    PushDelta(&decision_to_price, trace.signal_decision_ns,
              StageTimeNs(trace, StrategySubmitStageForTest::kPricePrepared));
    PushDelta(
        &price_to_signal_decision_log,
        StageTimeNs(trace, StrategySubmitStageForTest::kPricePrepared),
        StageTimeNs(trace, StrategySubmitStageForTest::kSignalDecisionLogged));
    PushDelta(
        &signal_decision_log_to_freshness,
        StageTimeNs(trace, StrategySubmitStageForTest::kSignalDecisionLogged),
        StageTimeNs(trace, StrategySubmitStageForTest::kFreshnessChecked));
    PushDelta(
        &freshness_to_quantity,
        StageTimeNs(trace, StrategySubmitStageForTest::kFreshnessChecked),
        StageTimeNs(trace, StrategySubmitStageForTest::kQuantityPrepared));
    PushDelta(&quantity_to_routes_refreshed,
              StageTimeNs(trace, StrategySubmitStageForTest::kQuantityPrepared),
              StageTimeNs(trace, StrategySubmitStageForTest::kRoutesRefreshed));
    PushDelta(&routes_refreshed_to_routes_selected,
              StageTimeNs(trace, StrategySubmitStageForTest::kRoutesRefreshed),
              StageTimeNs(trace, StrategySubmitStageForTest::kRoutesSelected));
    PushDelta(&routes_selected_to_risk,
              StageTimeNs(trace, StrategySubmitStageForTest::kRoutesSelected),
              StageTimeNs(trace, StrategySubmitStageForTest::kRiskChecked));
    PushDelta(
        &risk_to_group,
        StageTimeNs(trace, StrategySubmitStageForTest::kRiskChecked),
        StageTimeNs(trace, StrategySubmitStageForTest::kExecutionGroupReady));
    PushDelta(
        &group_to_route0_acquire_begin,
        StageTimeNs(trace, StrategySubmitStageForTest::kExecutionGroupReady),
        RouteStageTimeNs(
            trace, StrategySubmitStageForTest::kBeforeAcquireRiskSlot, 0));
    PushDelta(&route0_acquire_text,
              RouteStageTimeNs(
                  trace, StrategySubmitStageForTest::kBeforeAcquireRiskSlot, 0),
              RouteStageTimeNs(
                  trace, StrategySubmitStageForTest::kAfterAcquireRiskSlot, 0));
    PushDelta(&route0_acquire_done_to_place_begin,
              RouteStageTimeNs(
                  trace, StrategySubmitStageForTest::kAfterAcquireRiskSlot, 0),
              RouteStageTimeNs(
                  trace, StrategySubmitStageForTest::kBeforePlaceOrder, 0));
    PushDelta(&route0_place_order,
              RouteStageTimeNs(
                  trace, StrategySubmitStageForTest::kBeforePlaceOrder, 0),
              RouteStageTimeNs(
                  trace, StrategySubmitStageForTest::kAfterPlaceOrder, 0));
    PushDelta(&route0_after_place_to_submit_result,
              RouteStageTimeNs(trace,
                               StrategySubmitStageForTest::kAfterPlaceOrder, 0),
              RouteStageTimeNs(
                  trace, StrategySubmitStageForTest::kAfterSubmitResult, 0));
    if (route_count > 3) {
      PushDelta(&route0_place_to_route3_place,
                RouteStageTimeNs(
                    trace, StrategySubmitStageForTest::kAfterPlaceOrder, 0),
                RouteStageTimeNs(
                    trace, StrategySubmitStageForTest::kAfterPlaceOrder, 3));
      PushDelta(&route3_submit_result_to_done,
                RouteStageTimeNs(
                    trace, StrategySubmitStageForTest::kAfterSubmitResult, 3),
                StageTimeNs(trace, StrategySubmitStageForTest::kSubmitDone));
    }
  }

  void SetCounters(benchmark::State& state) const {
    SetPrefixedLatencyCounters(state, "decision_to_price", decision_to_price);
    SetPrefixedLatencyCounters(state, "price_to_signal_decision_log",
                               price_to_signal_decision_log);
    SetPrefixedLatencyCounters(state, "signal_decision_log_to_freshness",
                               signal_decision_log_to_freshness);
    SetPrefixedLatencyCounters(state, "freshness_to_quantity",
                               freshness_to_quantity);
    SetPrefixedLatencyCounters(state, "quantity_to_routes_refreshed",
                               quantity_to_routes_refreshed);
    SetPrefixedLatencyCounters(state, "routes_refreshed_to_routes_selected",
                               routes_refreshed_to_routes_selected);
    SetPrefixedLatencyCounters(state, "routes_selected_to_risk",
                               routes_selected_to_risk);
    SetPrefixedLatencyCounters(state, "risk_to_group", risk_to_group);
    SetPrefixedLatencyCounters(state, "group_to_route0_acquire_begin",
                               group_to_route0_acquire_begin);
    SetPrefixedLatencyCounters(state, "route0_acquire_text",
                               route0_acquire_text);
    SetPrefixedLatencyCounters(state, "route0_acquire_done_to_place_begin",
                               route0_acquire_done_to_place_begin);
    SetPrefixedLatencyCounters(state, "route0_place_order", route0_place_order);
    SetPrefixedLatencyCounters(state, "route0_after_place_to_submit_result",
                               route0_after_place_to_submit_result);
    SetPrefixedLatencyCounters(state, "route0_place_to_route3_place",
                               route0_place_to_route3_place);
    SetPrefixedLatencyCounters(state, "route3_submit_result_to_done",
                               route3_submit_result_to_done);
  }
};

void OnSignalTriggeredForBenchmark(
    const detail::StrategySignalTriggeredLogRecordForTest& record) noexcept {
  if (active_submit_trace == nullptr) {
    return;
  }
  active_submit_trace->signal_decision_ns = record.signal_decision_ns;
  active_submit_trace->signal_trigger_observer_ns =
      benchmarking::RealtimeNowNs();
}

void OnSignalDecisionOnlyForBenchmark(
    const detail::StrategySignalTriggeredLogRecordForTest& record) noexcept {
  if (active_submit_trace == nullptr) {
    return;
  }
  active_submit_trace->signal_decision_ns = record.signal_decision_ns;
}

void OnOrderSubmittedForBenchmark(
    const detail::StrategyOrderSubmittedLogRecordForTest& record) noexcept {
  if (active_submit_trace == nullptr || record.route_id >= kFanout) {
    return;
  }
  active_submit_trace->submitted_observer_ns[record.route_id] =
      benchmarking::RealtimeNowNs();
  ++active_submit_trace->submitted_calls;
}

void OnSubmitStageForBenchmark(
    const detail::StrategySubmitStageRecordForTest& record) noexcept {
  if (active_submit_trace == nullptr) {
    return;
  }
  const std::size_t stage_index = StageIndex(record.stage);
  if (stage_index >= active_submit_trace->stage_ns.size()) {
    return;
  }
  const std::int64_t now_ns = benchmarking::RealtimeNowNs();
  if (record.route_id < kFanout) {
    active_submit_trace->route_stage_ns[stage_index][record.route_id] = now_ns;
    return;
  }
  active_submit_trace->stage_ns[stage_index] = now_ns;
}

class StrategyLogHookScope {
 public:
  StrategyLogHookScope() noexcept {
    detail::SetStrategySignalTriggeredLogObserverForTest(
        OnSignalTriggeredForBenchmark);
    detail::SetStrategyOrderSubmittedLogObserverForTest(
        OnOrderSubmittedForBenchmark);
    detail::SetStrategySubmitStageObserverForTest(OnSubmitStageForBenchmark);
  }

  ~StrategyLogHookScope() {
    detail::SetStrategySignalTriggeredLogObserverForTest(nullptr);
    detail::SetStrategyOrderSubmittedLogObserverForTest(nullptr);
    detail::SetStrategySubmitStageObserverForTest(nullptr);
    active_submit_trace = nullptr;
  }

  StrategyLogHookScope(const StrategyLogHookScope&) = delete;
  StrategyLogHookScope& operator=(const StrategyLogHookScope&) = delete;
};

class StrategyDecisionHookScope {
 public:
  StrategyDecisionHookScope() noexcept {
    detail::SetStrategySignalTriggeredLogObserverForTest(
        OnSignalDecisionOnlyForBenchmark);
  }

  ~StrategyDecisionHookScope() {
    detail::SetStrategySignalTriggeredLogObserverForTest(nullptr);
    active_submit_trace = nullptr;
  }

  StrategyDecisionHookScope(const StrategyDecisionHookScope&) = delete;
  StrategyDecisionHookScope& operator=(const StrategyDecisionHookScope&) =
      delete;
};

struct SharedInstrumentedOrderSessionState {
  SubmitTrace* trace{nullptr};
};

struct InstrumentedOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
    std::int64_t send_local_ns{0};
  };

  explicit InstrumentedOrderSession(SharedInstrumentedOrderSessionState* shared)
      : state(shared) {}

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

  [[nodiscard]] std::uint16_t MaxOrderSessionFanout() const noexcept {
    return kFanout;
  }

  [[nodiscard]] std::uint16_t route_count() const noexcept {
    return kFanout;
  }

  void RefreshRouteStates() noexcept {}

  [[nodiscard]] bool RouteReady(std::uint16_t route_id) const noexcept {
    return route_id < kFanout;
  }

  SendResult PlaceOrder(const core::OrderPlaceRequest& request) noexcept {
    const std::int64_t now_ns = benchmarking::RealtimeNowNs();
    if (state != nullptr && state->trace != nullptr &&
        request.gateway_route_id < kFanout) {
      state->trace->place_enter_ns[request.gateway_route_id] = now_ns;
      ++state->trace->place_calls;
    }
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk, .send_local_ns = now_ns};
  }

  SendResult CancelOrder(const core::OrderCancelRequest&) noexcept {
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk, .send_local_ns = 0};
  }

  SharedInstrumentedOrderSessionState* state{};
  bool running{false};
};

using InstrumentedRuntime =
    core::TradingRuntime<Strategy, InstrumentedOrderSession,
                         market_data::RealtimeDataReader<>,
                         core::TradingRuntimeDiagnostics>;

class InstrumentedOrderGatewayClient {
 public:
  InstrumentedOrderGatewayClient(core::OrderGatewayClient client,
                                 SubmitTrace* trace) noexcept
      : client_(std::move(client)), trace_(trace) {}

  InstrumentedOrderGatewayClient(InstrumentedOrderGatewayClient&&) noexcept =
      default;
  InstrumentedOrderGatewayClient& operator=(
      InstrumentedOrderGatewayClient&&) noexcept = default;

  [[nodiscard]] bool Start() noexcept {
    return client_.Start();
  }
  void Stop() noexcept {
    client_.Stop();
  }
  [[nodiscard]] bool Ready() const noexcept {
    return client_.Ready();
  }
  [[nodiscard]] bool Running() const noexcept {
    return client_.Running();
  }
  [[nodiscard]] std::uint16_t MaxOrderSessionFanout() const noexcept {
    return client_.MaxOrderSessionFanout();
  }
  [[nodiscard]] std::uint16_t route_count() const noexcept {
    return client_.route_count();
  }
  void RefreshRouteStates() noexcept {
    client_.RefreshRouteStates();
  }
  [[nodiscard]] bool RouteReady(std::uint16_t route_id) const noexcept {
    return client_.RouteReady(route_id);
  }

  [[nodiscard]] core::OrderGatewaySendResult PlaceOrder(
      const core::OrderPlaceRequest& request) noexcept {
    const core::OrderGatewaySendResult sent = client_.PlaceOrder(request);
    if (sent.status == core::OrderGatewaySendStatus::kOk && trace_ != nullptr &&
        request.gateway_route_id < kFanout) {
      trace_->place_enter_ns[request.gateway_route_id] = sent.send_local_ns;
      ++trace_->place_calls;
    }
    return sent;
  }

  [[nodiscard]] core::OrderGatewaySendResult CancelOrder(
      const core::OrderCancelRequest& request) noexcept {
    return client_.CancelOrder(request);
  }

 private:
  core::OrderGatewayClient client_;
  SubmitTrace* trace_{nullptr};
};

using InstrumentedOrderGatewayRuntime =
    core::TradingRuntime<Strategy, InstrumentedOrderGatewayClient,
                         market_data::RealtimeDataReader<>,
                         core::TradingRuntimeDiagnostics>;

[[nodiscard]] Result<core::OrderGatewayClient> MakeStartedGatewayClient(
    std::uint64_t route_table_capacity, std::uint16_t route_count = kFanout) {
  static std::uint64_t next_instance_id = 0;
  const std::string shm_name =
      std::string{"aquila_submit_breakdown_"} +
      std::to_string(static_cast<unsigned long long>(::getpid())) + "_" +
      std::to_string(++next_instance_id);
  core::OrderGatewayShmConfig shm_config{
      .shm_name = shm_name,
      .create = true,
      .remove_existing = true,
      .route_count = route_count,
      .command_queue_capacity = kGatewayQueueCapacity,
      .event_queue_capacity = 64,
      .startup_ready_timeout_s = 1,
  };
  auto manager_result = core::OrderGatewayShmManager::Create(shm_config);
  if (!manager_result.ok) {
    Result<core::OrderGatewayClient> result;
    result.error = std::move(manager_result.error);
    return result;
  }
  for (std::uint16_t route = 0; route < route_count; ++route) {
    core::StoreOrderGatewayRouteState(manager_result.value.header(), route,
                                      core::OrderGatewayRouteState::kReady);
  }
  auto client_result = core::OrderGatewayClient::Attach(
      std::move(manager_result.value),
      core::OrderGatewayClientOptions{
          .route_table_capacity = route_table_capacity,
          .max_events_per_poll_per_route = 64,
      });
  if (!client_result.ok) {
    return client_result;
  }
  if (!client_result.value.Start()) {
    client_result.ok = false;
    client_result.error = "order gateway client start failed";
    return client_result;
  }
  const std::string normalized_name = "/" + shm_name;
  ::shm_unlink(normalized_name.c_str());
  return client_result;
}

[[nodiscard]] config::StrategyConfig RuntimeConfig(
    std::size_t order_capacity = kOrderCapacity) {
  config::StrategyConfig config;
  config.name = "lead_lag_submit_breakdown";
  config.strategy_id = kStrategyId;
  config.order_capacity = order_capacity;
  config.feedback.enabled = false;
  return config;
}

[[nodiscard]] Config BenchmarkLeadLagConfig(
    Exchange lag_exchange = Exchange::kGate) {
  const std::string exchange_symbol =
      lag_exchange == Exchange::kBitget ? "BASUSDT" : std::string{kSymbol};
  Config config;
  config.name = "lead_lag";
  config.version = "1.0";
  config.pairs.push_back(PairConfig{
      .symbol = std::string{kSymbol},
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
              .open_notional = 10.0,
              .trailing_stop = 0.05,
              .max_entry_spread = 0.02,
              .open_slippage_ticks = 10,
              .parallel = 1,
              .order_session_fanout = kFanout,
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
              .exchange_symbol = exchange_symbol,
              .price_tick = 0.000001,
              .price_decimal_places = 6,
              .quantity_step = 1.0,
              .quantity_decimal_places = 0,
              .min_quantity = 1.0,
              .max_quantity = 1'000'000.0,
              .notional_multiplier = 1.0,
              .lag_taker_fee = 0.0,
          },
  });
  return config;
}

[[nodiscard]] PairConfig LiveLikePairConfig(const LiveSymbolSpec& spec) {
  return PairConfig{
      .symbol = std::string{spec.symbol},
      .symbol_id = spec.symbol_id,
      .lead_exchange = Exchange::kBinance,
      .lag_exchange = Exchange::kGate,
      .lag_taker_fee = 0.00020,
      .max_lead_freshness_ms = 5,
      .max_lag_freshness_ms = 200,
      .trigger =
          TriggerConfig{
              .lead = 0.0025,
              .close = 0.0005,
              .lag_part = 0.5,
              .target_profit_rate = 0.0,
              .drift_period_ns = 60'000'000'000ULL,
              .drift_min_samples = 1,
              .drift_warmup_ns = 1,
              .quantile =
                  QuantileConfig{
                      .move = 0.75,
                      .up_min = 0.0,
                      .up_max = 0.02,
                      .down_min = -0.02,
                      .down_max = 0.0,
                      .precision = 0.000001,
                  },
              .drift_guard =
                  DriftGuardConfig{
                      .enabled = true,
                      .drift_instant = 0.015,
                      .ratio_std = 0.008,
                      .ratio_std_window_ns = 60'000'000'000ULL,
                      .drift_mean = 0.02,
                      .drift_mean_window_ns = 60'000'000'000ULL,
                  },
          },
      .execute =
          ExecuteConfig{
              .open_notional = 10.0,
              .trailing_stop = 0.01,
              .max_entry_spread = 0.01,
              .open_slippage_ticks = 10,
              .close_slippage_ticks = 10,
              .stoploss_slippage_ticks = 25,
              .close_retry_times = 2,
              .close_retry_slippage_step_ticks = 10,
              .parallel = 1,
              .order_session_fanout = kFanout,
          },
      .bbo_record =
          BboRecordConfig{
              .window_ns = 1'000'000'000ULL,
              .stats_window_ns = 30'000'000'000ULL,
          },
      .capacity =
          CapacityConfig{
              .extrema_window_capacity = 16'384,
              .move_queue_capacity = 16'384,
              .noise_window_capacity = 16'384,
              .spread_window_capacity = 16'384,
              .drift_guard_window_capacity = 1'024,
          },
      .lag_instrument =
          InstrumentMetadata{
              .symbol_id = spec.symbol_id,
              .exchange = Exchange::kGate,
              .exchange_symbol = std::string{spec.symbol},
              .price_tick = 0.000001,
              .price_decimal_places = 6,
              .quantity_step = 1.0,
              .quantity_decimal_places = 0,
              .min_quantity = 1.0,
              .max_quantity = 1'000'000.0,
              .notional_multiplier = 1.0,
              .lag_taker_fee = 0.00020,
          },
  };
}

[[nodiscard]] Config BenchmarkLiveLikeConfig(bool risk_enabled) {
  Config config;
  config.name = "lead_lag";
  config.version = "1.0";
  if (risk_enabled) {
    config.risk.max_gross_notional = 1000.0;
  }
  config.pairs.reserve(kLiveSymbolSpecs.size());
  for (const LiveSymbolSpec& spec : kLiveSymbolSpecs) {
    config.pairs.push_back(LiveLikePairConfig(spec));
  }
  return config;
}

void NormalizeActualConfigForBenchmark(Config* config) noexcept {
  if (config == nullptr) {
    return;
  }
  for (PairConfig& pair : config->pairs) {
    pair.trigger.drift_min_samples = 1;
    pair.trigger.drift_warmup_ns = 1;
    pair.capacity.drift_guard_window_capacity = 1'024;
  }
}

[[nodiscard]] ConfigResult LoadActualLiveConfigForBenchmark() {
  const auto catalog_result = ::aquila::config::LoadInstrumentCatalogFromCsv(
      std::filesystem::path{kActualLiveInstrumentCatalogPath});
  if (!catalog_result.ok) {
    ConfigResult result;
    result.error = catalog_result.error;
    return result;
  }
  ConfigResult config_result =
      LoadConfigFile(std::filesystem::path{kActualLiveLeadLagConfigPath},
                     catalog_result.value);
  if (config_result.ok) {
    NormalizeActualConfigForBenchmark(&config_result.value);
  }
  return config_result;
}

[[nodiscard]] ConfigResult LoadBitgetCombined46ConfigForBenchmark() {
  const auto catalog_result = ::aquila::config::LoadInstrumentCatalogFromCsv(
      std::filesystem::path{kActualLiveInstrumentCatalogPath});
  if (!catalog_result.ok) {
    ConfigResult result;
    result.error = catalog_result.error;
    return result;
  }
  ConfigResult config_result =
      LoadConfigFile(std::filesystem::path{kBitgetCombined46LeadLagConfigPath},
                     catalog_result.value);
  if (!config_result.ok) {
    return config_result;
  }
  NormalizeActualConfigForBenchmark(&config_result.value);
  for (PairConfig& pair : config_result.value.pairs) {
    pair.execute.order_session_fanout = kSingleRouteFanout;
  }
  return config_result;
}

[[nodiscard]] BookTicker TickerFor(std::int32_t symbol_id, Exchange exchange,
                                   std::int64_t local_ns, double bid_price,
                                   double ask_price) noexcept {
  return BookTicker{
      .id = local_ns,
      .symbol_id = symbol_id,
      .exchange = exchange,
      .exchange_ns = local_ns - 10,
      .local_ns = local_ns,
      .bid_price = bid_price,
      .bid_volume = 1.0,
      .ask_price = ask_price,
      .ask_volume = 1.0,
  };
}

[[nodiscard]] BookTicker Ticker(Exchange exchange, std::int64_t local_ns,
                                double bid_price, double ask_price) noexcept {
  return TickerFor(kSymbolId, exchange, local_ns, bid_price, ask_price);
}

template <typename RuntimeT>
[[nodiscard]] std::int64_t SeedBeforeOpenSignal(
    RuntimeT& runtime, std::int32_t symbol_id = kSymbolId,
    Exchange lag_exchange = Exchange::kGate) noexcept {
  const std::int64_t base_ns = benchmarking::RealtimeNowNs();
  runtime.HandleBookTickerForTest(
      TickerFor(symbol_id, lag_exchange, base_ns, 0.052000, 0.052020));
  runtime.HandleBookTickerForTest(TickerFor(
      symbol_id, Exchange::kBinance, base_ns + 1'000, 0.052000, 0.052010));
  return base_ns + 2'000;
}

[[nodiscard]] BookTicker OpenLongTriggerTicker(
    std::int64_t event_ns, std::int32_t symbol_id = kSymbolId) noexcept {
  return TickerFor(symbol_id, Exchange::kBinance, event_ns, 0.061000, 0.061010);
}

[[nodiscard]] BookTicker LiveLikeOpenLongTriggerTicker(
    std::int64_t event_ns, std::int32_t symbol_id = kSymbolId) noexcept {
  return TickerFor(symbol_id, Exchange::kBinance, event_ns, 0.052250, 0.052260);
}

[[nodiscard]] const PairConfig* FindPairConfig(const Config& config,
                                               std::int32_t symbol_id) {
  const auto it = std::find_if(config.pairs.begin(), config.pairs.end(),
                               [symbol_id](const PairConfig& pair) {
                                 return pair.symbol_id == symbol_id;
                               });
  return it == config.pairs.end() ? nullptr : &*it;
}

[[nodiscard]] const PairConfig* FindWarmupPairConfig(const Config& config) {
  return FindPairConfig(config, kWarmupSymbolId);
}

template <typename RuntimeT>
void HandleNonTriggeringPairUpdate(RuntimeT& runtime, const PairConfig& pair,
                                   std::int64_t event_ns,
                                   double price_offset = 0.0) noexcept {
  const double bid_price = 0.052000 + price_offset;
  runtime.HandleBookTickerForTest(TickerFor(pair.symbol_id, pair.lag_exchange,
                                            event_ns, bid_price,
                                            bid_price + 0.000020));
  runtime.HandleBookTickerForTest(TickerFor(pair.symbol_id, pair.lead_exchange,
                                            event_ns + 1'000, bid_price,
                                            bid_price + 0.000010));
}

template <typename RuntimeT>
void SeedAllPairsWithoutSignal(RuntimeT& runtime,
                               const Config& config) noexcept {
  std::int64_t event_ns = benchmarking::RealtimeNowNs();
  for (const PairConfig& pair : config.pairs) {
    HandleNonTriggeringPairUpdate(runtime, pair, event_ns);
    event_ns += 2'000;
  }
}

template <typename RuntimeT>
void ChurnAllPairsWithoutSignal(RuntimeT& runtime, const Config& config,
                                std::size_t sweep_count) noexcept {
  for (std::size_t sweep = 0; sweep < sweep_count; ++sweep) {
    std::int64_t event_ns = benchmarking::RealtimeNowNs();
    for (const PairConfig& pair : config.pairs) {
      const double price_offset =
          ((static_cast<std::size_t>(pair.symbol_id) + sweep) & 1U) == 0
              ? 0.000001
              : 0.0;
      HandleNonTriggeringPairUpdate(runtime, pair, event_ns, price_offset);
      event_ns += 2'000;
    }
  }
}

template <typename RuntimeT>
void PrefillLiveLikeOpenSignals(RuntimeT& runtime,
                                std::size_t signal_count) noexcept {
  std::size_t emitted = 0;
  for (const LiveSymbolSpec& spec : kLiveSymbolSpecs) {
    if (spec.symbol_id == kSymbolId) {
      continue;
    }
    const std::int64_t trigger_event_ns =
        SeedBeforeOpenSignal(runtime, spec.symbol_id);
    runtime.HandleBookTickerForTest(
        LiveLikeOpenLongTriggerTicker(trigger_event_ns, spec.symbol_id));
    ++emitted;
    if (emitted >= signal_count) {
      return;
    }
  }
}

void BM_LeadLagSubmitPathBreakdownSyntheticFanout4(benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  StrategyLogHookScope hooks;

  std::vector<std::uint64_t> full_samples_ns;
  std::vector<std::uint64_t> decision_to_signal_log_done_ns;
  std::vector<std::uint64_t> signal_log_to_group_ready_ns;
  std::vector<std::uint64_t> group_ready_to_route0_place_ns;
  std::vector<std::uint64_t> route0_to_route3_place_ns;
  std::vector<std::uint64_t> route3_place_to_done_ns;
  std::array<std::vector<std::uint64_t>, kFanout> decision_to_route_place_ns;
  full_samples_ns.reserve(kLatencyIterations);
  decision_to_signal_log_done_ns.reserve(kLatencyIterations);
  signal_log_to_group_ready_ns.reserve(kLatencyIterations);
  group_ready_to_route0_place_ns.reserve(kLatencyIterations);
  route0_to_route3_place_ns.reserve(kLatencyIterations);
  route3_place_to_done_ns.reserve(kLatencyIterations);
  for (auto& samples : decision_to_route_place_ns) {
    samples.reserve(kLatencyIterations);
  }

  for (auto _ : state) {
    state.PauseTiming();
    SubmitTrace trace;
    SharedInstrumentedOrderSessionState session_state{.trace = &trace};
    auto runtime_result = InstrumentedRuntime::CreateForTest(
        RuntimeConfig(),
        [&session_state] { return InstrumentedOrderSession{&session_state}; },
        BenchmarkLeadLagConfig());
    if (!runtime_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(runtime_result.error.c_str());
      return;
    }

    InstrumentedRuntime& runtime = *runtime_result.value;
    const std::int64_t trigger_event_ns = SeedBeforeOpenSignal(runtime);
    const BookTicker trigger_ticker = OpenLongTriggerTicker(trigger_event_ns);
    trace.Reset();
    active_submit_trace = &trace;
    state.ResumeTiming();

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    runtime.HandleBookTickerForTest(trigger_ticker);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    trace.handle_end_ns = benchmarking::RealtimeNowNs();
    state.PauseTiming();

    if (trace.signal_decision_ns == 0 || trace.place_calls != kFanout ||
        trace.submitted_calls != kFanout || trace.place_enter_ns[0] == 0 ||
        trace.place_enter_ns[3] == 0) {
      state.ResumeTiming();
      state.SkipWithError(
          "lead lag submit path did not emit four child orders");
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    full_samples_ns.push_back(elapsed_ns);
    decision_to_signal_log_done_ns.push_back(
        DeltaNs(trace.signal_decision_ns, trace.signal_trigger_observer_ns));
    signal_log_to_group_ready_ns.push_back(DeltaNs(
        trace.signal_trigger_observer_ns,
        StageTimeNs(trace, StrategySubmitStageForTest::kExecutionGroupReady)));
    group_ready_to_route0_place_ns.push_back(DeltaNs(
        StageTimeNs(trace, StrategySubmitStageForTest::kExecutionGroupReady),
        trace.place_enter_ns[0]));
    route0_to_route3_place_ns.push_back(
        DeltaNs(trace.place_enter_ns[0], trace.place_enter_ns[3]));
    route3_place_to_done_ns.push_back(
        DeltaNs(trace.place_enter_ns[3], trace.handle_end_ns));
    for (std::uint16_t route = 0; route < kFanout; ++route) {
      decision_to_route_place_ns[route].push_back(
          DeltaNs(trace.signal_decision_ns, trace.place_enter_ns[route]));
    }

    benchmark::DoNotOptimize(trace.place_calls);
    benchmark::DoNotOptimize(trace.submitted_calls);
    active_submit_trace = nullptr;
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(full_samples_ns),
                                              "parents", state.iterations());
  SetPrefixedLatencyCounters(state, "decision_to_signal_log_done",
                             decision_to_signal_log_done_ns);
  SetPrefixedLatencyCounters(state, "signal_log_to_group_ready",
                             signal_log_to_group_ready_ns);
  SetPrefixedLatencyCounters(state, "group_ready_to_route0_place",
                             group_ready_to_route0_place_ns);
  SetPrefixedLatencyCounters(state, "route0_place_to_route3_place",
                             route0_to_route3_place_ns);
  SetPrefixedLatencyCounters(state, "route3_place_to_done",
                             route3_place_to_done_ns);
  for (std::uint16_t route = 0; route < kFanout; ++route) {
    SetPrefixedLatencyCounters(
        state,
        std::string("decision_to_route") + std::to_string(route) + "_place",
        decision_to_route_place_ns[route]);
  }
}

void BM_LeadLagSubmitPathBreakdownOrderGatewaySyntheticFanout4(
    benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  StrategyLogHookScope hooks;

  std::vector<std::uint64_t> full_samples_ns;
  std::vector<std::uint64_t> decision_to_signal_log_done_ns;
  std::vector<std::uint64_t> signal_log_to_group_ready_ns;
  std::vector<std::uint64_t> group_ready_to_route0_enqueue_ns;
  std::vector<std::uint64_t> route0_to_route3_enqueue_ns;
  std::vector<std::uint64_t> route3_enqueue_to_done_ns;
  std::array<std::vector<std::uint64_t>, kFanout> decision_to_route_enqueue_ns;
  full_samples_ns.reserve(kLatencyIterations);
  decision_to_signal_log_done_ns.reserve(kLatencyIterations);
  signal_log_to_group_ready_ns.reserve(kLatencyIterations);
  group_ready_to_route0_enqueue_ns.reserve(kLatencyIterations);
  route0_to_route3_enqueue_ns.reserve(kLatencyIterations);
  route3_enqueue_to_done_ns.reserve(kLatencyIterations);
  for (auto& samples : decision_to_route_enqueue_ns) {
    samples.reserve(kLatencyIterations);
  }

  for (auto _ : state) {
    state.PauseTiming();
    SubmitTrace trace;
    auto client_result = MakeStartedGatewayClient(kFanout + 16);
    if (!client_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(client_result.error.c_str());
      return;
    }
    auto runtime_result = InstrumentedOrderGatewayRuntime::CreateForTest(
        RuntimeConfig(),
        [&client = client_result.value, &trace]() mutable {
          return InstrumentedOrderGatewayClient{std::move(client), &trace};
        },
        BenchmarkLeadLagConfig());
    if (!runtime_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(runtime_result.error.c_str());
      return;
    }

    InstrumentedOrderGatewayRuntime& runtime = *runtime_result.value;
    const std::int64_t trigger_event_ns = SeedBeforeOpenSignal(runtime);
    const BookTicker trigger_ticker = OpenLongTriggerTicker(trigger_event_ns);
    trace.Reset();
    active_submit_trace = &trace;
    state.ResumeTiming();

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    runtime.HandleBookTickerForTest(trigger_ticker);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    trace.handle_end_ns = benchmarking::RealtimeNowNs();
    state.PauseTiming();

    if (trace.signal_decision_ns == 0 || trace.place_calls != kFanout ||
        trace.submitted_calls != kFanout || trace.place_enter_ns[0] == 0 ||
        trace.place_enter_ns[3] == 0) {
      state.ResumeTiming();
      state.SkipWithError(
          "lead lag order gateway path did not enqueue four child orders");
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    full_samples_ns.push_back(elapsed_ns);
    decision_to_signal_log_done_ns.push_back(
        DeltaNs(trace.signal_decision_ns, trace.signal_trigger_observer_ns));
    signal_log_to_group_ready_ns.push_back(DeltaNs(
        trace.signal_trigger_observer_ns,
        StageTimeNs(trace, StrategySubmitStageForTest::kExecutionGroupReady)));
    group_ready_to_route0_enqueue_ns.push_back(DeltaNs(
        StageTimeNs(trace, StrategySubmitStageForTest::kExecutionGroupReady),
        trace.place_enter_ns[0]));
    route0_to_route3_enqueue_ns.push_back(
        DeltaNs(trace.place_enter_ns[0], trace.place_enter_ns[3]));
    route3_enqueue_to_done_ns.push_back(
        DeltaNs(trace.place_enter_ns[3], trace.handle_end_ns));
    for (std::uint16_t route = 0; route < kFanout; ++route) {
      decision_to_route_enqueue_ns[route].push_back(
          DeltaNs(trace.signal_decision_ns, trace.place_enter_ns[route]));
    }

    benchmark::DoNotOptimize(trace.place_calls);
    benchmark::DoNotOptimize(trace.submitted_calls);
    active_submit_trace = nullptr;
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(full_samples_ns),
                                              "parents", state.iterations());
  SetPrefixedLatencyCounters(state, "decision_to_signal_log_done",
                             decision_to_signal_log_done_ns);
  SetPrefixedLatencyCounters(state, "signal_log_to_group_ready",
                             signal_log_to_group_ready_ns);
  SetPrefixedLatencyCounters(state, "group_ready_to_route0_enqueue",
                             group_ready_to_route0_enqueue_ns);
  SetPrefixedLatencyCounters(state, "route0_enqueue_to_route3_enqueue",
                             route0_to_route3_enqueue_ns);
  SetPrefixedLatencyCounters(state, "route3_enqueue_to_done",
                             route3_enqueue_to_done_ns);
  for (std::uint16_t route = 0; route < kFanout; ++route) {
    SetPrefixedLatencyCounters(
        state,
        std::string("decision_to_route") + std::to_string(route) + "_enqueue",
        decision_to_route_enqueue_ns[route]);
  }
}

void RunLeadLagSubmitPathBreakdownOrderGatewayConfig(
    benchmark::State& state, const Config& benchmark_config,
    std::size_t prefill_open_signal_count = 0) {
  benchmarking::EnsureLoggingStarted();
  StrategyLogHookScope hooks;

  std::vector<std::uint64_t> full_samples_ns;
  std::vector<std::uint64_t> decision_to_signal_log_done_ns;
  std::vector<std::uint64_t> signal_log_to_group_ready_ns;
  std::vector<std::uint64_t> group_ready_to_route0_enqueue_ns;
  std::vector<std::uint64_t> route0_to_route3_enqueue_ns;
  std::vector<std::uint64_t> route3_enqueue_to_done_ns;
  std::array<std::vector<std::uint64_t>, kFanout> decision_to_route_enqueue_ns;
  SubmitStageSamples stage_samples;
  full_samples_ns.reserve(kLatencyIterations);
  decision_to_signal_log_done_ns.reserve(kLatencyIterations);
  signal_log_to_group_ready_ns.reserve(kLatencyIterations);
  group_ready_to_route0_enqueue_ns.reserve(kLatencyIterations);
  route0_to_route3_enqueue_ns.reserve(kLatencyIterations);
  route3_enqueue_to_done_ns.reserve(kLatencyIterations);
  stage_samples.Reserve(kLatencyIterations);
  for (auto& samples : decision_to_route_enqueue_ns) {
    samples.reserve(kLatencyIterations);
  }

  for (auto _ : state) {
    state.PauseTiming();
    SubmitTrace trace;
    const std::uint64_t route_table_capacity =
        static_cast<std::uint64_t>(prefill_open_signal_count) * kFanout +
        kFanout + 16;
    auto client_result = MakeStartedGatewayClient(route_table_capacity);
    if (!client_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(client_result.error.c_str());
      return;
    }
    auto runtime_result = InstrumentedOrderGatewayRuntime::CreateForTest(
        RuntimeConfig(128),
        [&client = client_result.value, &trace]() mutable {
          return InstrumentedOrderGatewayClient{std::move(client), &trace};
        },
        benchmark_config);
    if (!runtime_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(runtime_result.error.c_str());
      return;
    }

    InstrumentedOrderGatewayRuntime& runtime = *runtime_result.value;
    PrefillLiveLikeOpenSignals(runtime, prefill_open_signal_count);
    const std::int64_t trigger_event_ns =
        SeedBeforeOpenSignal(runtime, kSymbolId);
    const BookTicker trigger_ticker =
        LiveLikeOpenLongTriggerTicker(trigger_event_ns, kSymbolId);
    trace.Reset();
    active_submit_trace = &trace;
    state.ResumeTiming();

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    runtime.HandleBookTickerForTest(trigger_ticker);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    trace.handle_end_ns = benchmarking::RealtimeNowNs();
    state.PauseTiming();

    if (trace.signal_decision_ns == 0 || trace.place_calls != kFanout ||
        trace.submitted_calls != kFanout || trace.place_enter_ns[0] == 0 ||
        trace.place_enter_ns[3] == 0 ||
        StageTimeNs(trace, StrategySubmitStageForTest::kSubmitDone) == 0) {
      const std::string error =
          std::string{
              "live-like lead lag order gateway path did not enqueue four "
              "child orders: signal_decision_ns="} +
          std::to_string(trace.signal_decision_ns) +
          " place_calls=" + std::to_string(trace.place_calls) +
          " submitted_calls=" + std::to_string(trace.submitted_calls) +
          " route0_enqueue_ns=" + std::to_string(trace.place_enter_ns[0]) +
          " route3_enqueue_ns=" + std::to_string(trace.place_enter_ns[3]) +
          " submit_done_ns=" +
          std::to_string(
              StageTimeNs(trace, StrategySubmitStageForTest::kSubmitDone));
      state.ResumeTiming();
      state.SkipWithError(error.c_str());
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    full_samples_ns.push_back(elapsed_ns);
    stage_samples.Push(trace);
    decision_to_signal_log_done_ns.push_back(
        DeltaNs(trace.signal_decision_ns, trace.signal_trigger_observer_ns));
    signal_log_to_group_ready_ns.push_back(DeltaNs(
        trace.signal_trigger_observer_ns,
        StageTimeNs(trace, StrategySubmitStageForTest::kExecutionGroupReady)));
    group_ready_to_route0_enqueue_ns.push_back(DeltaNs(
        StageTimeNs(trace, StrategySubmitStageForTest::kExecutionGroupReady),
        trace.place_enter_ns[0]));
    route0_to_route3_enqueue_ns.push_back(
        DeltaNs(trace.place_enter_ns[0], trace.place_enter_ns[3]));
    route3_enqueue_to_done_ns.push_back(
        DeltaNs(trace.place_enter_ns[3], trace.handle_end_ns));
    for (std::uint16_t route = 0; route < kFanout; ++route) {
      decision_to_route_enqueue_ns[route].push_back(
          DeltaNs(trace.signal_decision_ns, trace.place_enter_ns[route]));
    }

    benchmark::DoNotOptimize(trace.place_calls);
    benchmark::DoNotOptimize(trace.submitted_calls);
    active_submit_trace = nullptr;
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(full_samples_ns),
                                              "parents", state.iterations());
  state.counters["prefill_open_signals"] =
      static_cast<double>(prefill_open_signal_count);
  SetPrefixedLatencyCounters(state, "decision_to_signal_log_done",
                             decision_to_signal_log_done_ns);
  SetPrefixedLatencyCounters(state, "signal_log_to_group_ready",
                             signal_log_to_group_ready_ns);
  SetPrefixedLatencyCounters(state, "group_ready_to_route0_enqueue",
                             group_ready_to_route0_enqueue_ns);
  SetPrefixedLatencyCounters(state, "route0_enqueue_to_route3_enqueue",
                             route0_to_route3_enqueue_ns);
  SetPrefixedLatencyCounters(state, "route3_enqueue_to_done",
                             route3_enqueue_to_done_ns);
  for (std::uint16_t route = 0; route < kFanout; ++route) {
    SetPrefixedLatencyCounters(
        state,
        std::string("decision_to_route") + std::to_string(route) + "_enqueue",
        decision_to_route_enqueue_ns[route]);
  }
  stage_samples.SetCounters(state);
}

void RunLeadLagBitget46Fanout1ChurnSubmitBreakdown(benchmark::State& state,
                                                   bool warm_before_target,
                                                   bool collect_stage_trace) {
  benchmarking::EnsureLoggingStarted();
  NOVA_INFO("lead_lag_submit_breakdown_logger_warmup");
  std::unique_ptr<StrategyLogHookScope> stage_hooks;
  std::unique_ptr<StrategyDecisionHookScope> decision_hook;
  if (collect_stage_trace) {
    stage_hooks = std::make_unique<StrategyLogHookScope>();
  } else {
    decision_hook = std::make_unique<StrategyDecisionHookScope>();
  }

  ConfigResult config_result = LoadBitgetCombined46ConfigForBenchmark();
  if (!config_result.ok) {
    state.SkipWithError(config_result.error.c_str());
    return;
  }
  const Config& benchmark_config = config_result.value;
  const PairConfig* target_pair = FindPairConfig(benchmark_config, kSymbolId);
  const PairConfig* warmup_pair = FindWarmupPairConfig(benchmark_config);
  if (benchmark_config.pairs.size() != 46 || target_pair == nullptr ||
      warmup_pair == nullptr) {
    state.SkipWithError(
        "Bitget combined46 benchmark config is missing required pairs");
    return;
  }

  const std::size_t churn_sweeps = static_cast<std::size_t>(state.range(0));
  std::vector<std::uint64_t> full_samples_ns;
  std::vector<std::uint64_t> decision_to_signal_log_done_ns;
  std::vector<std::uint64_t> signal_log_done_to_price_ns;
  std::vector<std::uint64_t> signal_log_to_group_ready_ns;
  std::vector<std::uint64_t> group_ready_to_request_timestamp_ns;
  std::vector<std::uint64_t> decision_to_request_timestamp_ns;
  std::vector<std::uint64_t> before_place_to_request_timestamp_ns;
  std::vector<std::uint64_t> request_timestamp_to_place_return_ns;
  std::vector<std::uint64_t> decision_to_place_return_ns;
  std::vector<std::uint64_t> request_timestamp_to_submitted_log_done_ns;
  std::vector<std::uint64_t> submitted_log_to_handle_end_ns;
  std::vector<std::uint64_t> decision_to_handle_end_ns;
  SubmitStageSamples stage_samples;
  full_samples_ns.reserve(kColdSubmitBreakdownIterations);
  decision_to_signal_log_done_ns.reserve(kColdSubmitBreakdownIterations);
  signal_log_done_to_price_ns.reserve(kColdSubmitBreakdownIterations);
  signal_log_to_group_ready_ns.reserve(kColdSubmitBreakdownIterations);
  group_ready_to_request_timestamp_ns.reserve(kColdSubmitBreakdownIterations);
  decision_to_request_timestamp_ns.reserve(kColdSubmitBreakdownIterations);
  before_place_to_request_timestamp_ns.reserve(kColdSubmitBreakdownIterations);
  request_timestamp_to_place_return_ns.reserve(kColdSubmitBreakdownIterations);
  decision_to_place_return_ns.reserve(kColdSubmitBreakdownIterations);
  request_timestamp_to_submitted_log_done_ns.reserve(
      kColdSubmitBreakdownIterations);
  submitted_log_to_handle_end_ns.reserve(kColdSubmitBreakdownIterations);
  decision_to_handle_end_ns.reserve(kColdSubmitBreakdownIterations);
  stage_samples.Reserve(kColdSubmitBreakdownIterations);

  for (auto _ : state) {
    state.PauseTiming();
    SubmitTrace trace;
    auto client_result = MakeStartedGatewayClient(
        /*route_table_capacity=*/32, kSingleRouteFanout);
    if (!client_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(client_result.error.c_str());
      return;
    }
    auto runtime_result = InstrumentedOrderGatewayRuntime::CreateForTest(
        RuntimeConfig(128),
        [&client = client_result.value, &trace]() mutable {
          return InstrumentedOrderGatewayClient{std::move(client), &trace};
        },
        benchmark_config);
    if (!runtime_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(runtime_result.error.c_str());
      return;
    }

    InstrumentedOrderGatewayRuntime& runtime = *runtime_result.value;
    SeedAllPairsWithoutSignal(runtime, benchmark_config);
    ChurnAllPairsWithoutSignal(runtime, benchmark_config, churn_sweeps);
    if (warm_before_target) {
      runtime.HandleBookTickerForTest(LiveLikeOpenLongTriggerTicker(
          benchmarking::RealtimeNowNs(), warmup_pair->symbol_id));
      if (trace.place_calls != kSingleRouteFanout) {
        state.ResumeTiming();
        state.SkipWithError(
            "Bitget combined46 warmup did not submit one child order");
        return;
      }
    }

    trace.Reset();
    active_submit_trace = &trace;
    const BookTicker trigger_ticker =
        TickerFor(target_pair->symbol_id, target_pair->lead_exchange,
                  benchmarking::RealtimeNowNs(), 0.052250, 0.052260);
    state.ResumeTiming();

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    runtime.HandleBookTickerForTest(trigger_ticker);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    trace.handle_end_ns = benchmarking::RealtimeNowNs();
    state.PauseTiming();

    const std::int64_t place_return_ns =
        collect_stage_trace
            ? RouteStageTimeNs(trace,
                               StrategySubmitStageForTest::kAfterPlaceOrder, 0)
            : 0;
    if (trace.signal_decision_ns == 0 ||
        trace.place_calls != kSingleRouteFanout ||
        trace.place_enter_ns[0] == 0 || trace.place_enter_ns[1] != 0 ||
        (collect_stage_trace &&
         (trace.submitted_calls != kSingleRouteFanout || place_return_ns == 0 ||
          StageTimeNs(trace, StrategySubmitStageForTest::kSubmitDone) == 0))) {
      const std::string error = fmt::format(
          "Bitget combined46 fanout1 submit failed: warm={} sweeps={} "
          "signal_decision_ns={} place_calls={} submitted_calls={} "
          "route0_request_ns={} route0_place_return_ns={} route1_request_ns={} "
          "submit_done_ns={}",
          warm_before_target, churn_sweeps, trace.signal_decision_ns,
          trace.place_calls, trace.submitted_calls, trace.place_enter_ns[0],
          place_return_ns, trace.place_enter_ns[1],
          StageTimeNs(trace, StrategySubmitStageForTest::kSubmitDone));
      active_submit_trace = nullptr;
      state.ResumeTiming();
      state.SkipWithError(error.c_str());
      return;
    }

    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    full_samples_ns.push_back(elapsed_ns);
    PushDelta(&decision_to_request_timestamp_ns, trace.signal_decision_ns,
              trace.place_enter_ns[0]);
    PushDelta(&decision_to_handle_end_ns, trace.signal_decision_ns,
              trace.handle_end_ns);
    if (collect_stage_trace) {
      stage_samples.Push(trace, kSingleRouteFanout);
      PushDelta(&decision_to_signal_log_done_ns, trace.signal_decision_ns,
                trace.signal_trigger_observer_ns);
      PushDelta(&signal_log_done_to_price_ns, trace.signal_trigger_observer_ns,
                StageTimeNs(trace, StrategySubmitStageForTest::kPricePrepared));
      PushDelta(
          &signal_log_to_group_ready_ns, trace.signal_trigger_observer_ns,
          StageTimeNs(trace, StrategySubmitStageForTest::kExecutionGroupReady));
      PushDelta(
          &group_ready_to_request_timestamp_ns,
          StageTimeNs(trace, StrategySubmitStageForTest::kExecutionGroupReady),
          trace.place_enter_ns[0]);
      PushDelta(&before_place_to_request_timestamp_ns,
                RouteStageTimeNs(
                    trace, StrategySubmitStageForTest::kBeforePlaceOrder, 0),
                trace.place_enter_ns[0]);
      PushDelta(&request_timestamp_to_place_return_ns, trace.place_enter_ns[0],
                place_return_ns);
      PushDelta(&decision_to_place_return_ns, trace.signal_decision_ns,
                place_return_ns);
      PushDelta(&request_timestamp_to_submitted_log_done_ns,
                trace.place_enter_ns[0], trace.submitted_observer_ns[0]);
      PushDelta(&submitted_log_to_handle_end_ns, trace.submitted_observer_ns[0],
                trace.handle_end_ns);
    }

    benchmark::DoNotOptimize(trace.place_calls);
    benchmark::DoNotOptimize(trace.submitted_calls);
    active_submit_trace = nullptr;
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(full_samples_ns),
                                              "parents", state.iterations());
  state.counters["pairs"] = static_cast<double>(benchmark_config.pairs.size());
  state.counters["fanout"] = kSingleRouteFanout;
  state.counters["churn_sweeps"] = static_cast<double>(churn_sweeps);
  state.counters["churn_updates"] =
      static_cast<double>(churn_sweeps * benchmark_config.pairs.size() * 2);
  state.counters["warm_before_target"] = warm_before_target ? 1.0 : 0.0;
  state.counters["stage_trace_enabled"] = collect_stage_trace ? 1.0 : 0.0;
  SetPrefixedLatencyCounters(state, "decision_to_signal_log_done",
                             decision_to_signal_log_done_ns);
  SetPrefixedLatencyCounters(state, "signal_log_done_to_price",
                             signal_log_done_to_price_ns);
  SetPrefixedLatencyCounters(state, "signal_log_to_group_ready",
                             signal_log_to_group_ready_ns);
  SetPrefixedLatencyCounters(state, "group_ready_to_request_timestamp",
                             group_ready_to_request_timestamp_ns);
  SetPrefixedLatencyCounters(state, "decision_to_request_timestamp",
                             decision_to_request_timestamp_ns);
  SetPrefixedLatencyCounters(state, "before_place_to_request_timestamp",
                             before_place_to_request_timestamp_ns);
  SetPrefixedLatencyCounters(state, "request_timestamp_to_place_return",
                             request_timestamp_to_place_return_ns);
  SetPrefixedLatencyCounters(state, "decision_to_place_return",
                             decision_to_place_return_ns);
  SetPrefixedLatencyCounters(state, "request_timestamp_to_submitted_log_done",
                             request_timestamp_to_submitted_log_done_ns);
  SetPrefixedLatencyCounters(state, "submitted_log_to_handle_end",
                             submitted_log_to_handle_end_ns);
  SetPrefixedLatencyCounters(state, "decision_to_handle_end",
                             decision_to_handle_end_ns);
  if (collect_stage_trace) {
    stage_samples.SetCounters(state);
  }
}

void BM_LeadLagSubmitPathBreakdownOrderGatewayBitget46Fanout1Churn(
    benchmark::State& state) {
  RunLeadLagBitget46Fanout1ChurnSubmitBreakdown(state,
                                                /*warm_before_target=*/false,
                                                /*collect_stage_trace=*/true);
}

void BM_LeadLagSubmitPathBreakdownOrderGatewayBitget46Fanout1Warm(
    benchmark::State& state) {
  RunLeadLagBitget46Fanout1ChurnSubmitBreakdown(state,
                                                /*warm_before_target=*/true,
                                                /*collect_stage_trace=*/true);
}

void BM_LeadLagSubmitPathBreakdownOrderGatewayBitget46Fanout1EndpointOnly(
    benchmark::State& state) {
  RunLeadLagBitget46Fanout1ChurnSubmitBreakdown(state,
                                                /*warm_before_target=*/false,
                                                /*collect_stage_trace=*/false);
}

void BM_LeadLagSubmitPathBreakdownOrderGatewayLiveLike30RiskOff(
    benchmark::State& state) {
  const Config config = BenchmarkLiveLikeConfig(/*risk_enabled=*/false);
  RunLeadLagSubmitPathBreakdownOrderGatewayConfig(state, config);
}

void BM_LeadLagSubmitPathBreakdownOrderGatewayLiveLike30RiskOn(
    benchmark::State& state) {
  const Config config = BenchmarkLiveLikeConfig(/*risk_enabled=*/true);
  RunLeadLagSubmitPathBreakdownOrderGatewayConfig(state, config);
}

void BM_LeadLagSubmitPathBreakdownOrderGatewayLiveLike30RiskOnPrefill20(
    benchmark::State& state) {
  const Config config = BenchmarkLiveLikeConfig(/*risk_enabled=*/true);
  RunLeadLagSubmitPathBreakdownOrderGatewayConfig(
      state, config, /*prefill_open_signal_count=*/20);
}

void BM_LeadLagSubmitPathBreakdownOrderGatewayActualConfigRiskOnBas(
    benchmark::State& state) {
  ConfigResult config_result = LoadActualLiveConfigForBenchmark();
  if (!config_result.ok) {
    state.SkipWithError(config_result.error.c_str());
    return;
  }
  RunLeadLagSubmitPathBreakdownOrderGatewayConfig(state, config_result.value);
}

void BM_LeadLagCurrentGlobalRiskTotalsActualConfig(benchmark::State& state) {
  ConfigResult config_result = LoadActualLiveConfigForBenchmark();
  if (!config_result.ok) {
    state.SkipWithError(config_result.error.c_str());
    return;
  }
  Strategy strategy{std::move(config_result.value)};
  RunManualLatencyBenchmark(
      state,
      [&] {
        auto totals = strategy.CurrentGlobalRiskTotalsForTest();
        benchmark::DoNotOptimize(totals.first);
        benchmark::DoNotOptimize(totals.second);
      },
      "pairs", strategy.config().pairs.size());
}

void BM_LogStrategySignalTriggeredSynthetic(benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  const SignalTiming timing = SyntheticTiming();
  RunManualLatencyBenchmark(state, [&] {
    detail::LogStrategySignalTriggered(Exchange::kBinance, kSymbolId, timing,
                                       kSymbol, kSymbolId, PairRole::kLead,
                                       SignalAction::kOpenLong, OrderSide::kBuy,
                                       false, 0, 0.052035);
  });
}

void BM_LogStrategyOrderIntentSynthetic(benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  const SignalTiming timing = SyntheticTiming();
  RunManualLatencyBenchmark(state, [&] {
    detail::LogStrategyOrderIntent(
        timing, kSymbol, kSymbolId, SignalAction::kOpenLong, OrderSide::kBuy,
        false, 0, 1.0, 0.052035, 0.052045, 10, 0.000001, 10.0, 0.052045, 0);
  });
}

void BM_LogStrategyOrderSubmittedSynthetic(benchmark::State& state) {
  benchmarking::EnsureLoggingStarted();
  const SignalTiming timing = SyntheticTiming();
  const detail::StrategyOrderPositionLogFields position =
      SyntheticPositionLog();
  std::uint64_t local_order_id = 432345564227567625ULL;
  RunManualLatencyBenchmark(state, [&] {
    detail::LogStrategyOrderSubmitted(
        local_order_id++, 1, 0, Exchange::kBinance, kSymbolId, timing, kSymbol,
        kSymbolId, PairRole::kLead, "entry", SignalAction::kOpenLong,
        OrderSide::kBuy, false, position, 1.0, 0.052035, 0.052045, 10, 0.000001,
        10.0, 0.052045, 1, core::OrderPlaceStatus::kOk);
  });
}

void BM_OrderDecimalPreparePriceQuantityAndTextSynthetic(
    benchmark::State& state) {
  constexpr std::int32_t kPriceDecimalPlaces = 6;
  constexpr std::int32_t kQuantityDecimalPlaces = 0;
  constexpr double kRawPrice = 0.052035;
  constexpr double kPriceTick = 0.000001;
  constexpr double kOpenNotional = 10.0;
  // Keep the historical benchmark name so before/after runs compare the same
  // submit-path stage. The candidate intentionally removes text formatting.
  RunManualLatencyBenchmark(state, [&] {
    const double order_price = kRawPrice + 10.0 * kPriceTick;
    const auto price_units = static_cast<std::int64_t>(
        std::llround(order_price * core::Pow10Int64(kPriceDecimalPlaces)));
    const auto open_notional_units = static_cast<std::int64_t>(
        std::llround(kOpenNotional * core::Pow10Int64(0)));
    const core::OpenQuantityUnitsResult quantity =
        core::CalculateOpenQuantityUnits(core::OpenQuantityUnitsInput{
            .notional_units = open_notional_units,
            .notional_decimal_places = 0,
            .price_units = price_units,
            .price_decimal_places = kPriceDecimalPlaces,
            .multiplier_units = 1,
            .multiplier_decimal_places = 0,
            .quantity_decimal_places = kQuantityDecimalPlaces,
            .quantity_step_units = 1,
            .min_quantity_units = 1,
            .max_quantity_units = 1'000'000,
        });
    double price = static_cast<double>(price_units) /
                   core::Pow10Int64(kPriceDecimalPlaces);
    double qty = static_cast<double>(quantity.quantity_units) /
                 core::Pow10Int64(kQuantityDecimalPlaces);
    std::int64_t quantity_units = quantity.quantity_units;
    benchmark::DoNotOptimize(price);
    benchmark::DoNotOptimize(qty);
    benchmark::DoNotOptimize(quantity_units);
  });
}

struct FakeOrderSession {
  enum class SendStatus : std::uint8_t { kOk, kRejected };

  struct SendResult {
    SendStatus status{SendStatus::kRejected};
    std::int64_t send_local_ns{0};
  };

  std::uint64_t place_calls{0};
  std::uint64_t last_place_local_order_id{0};

  SendResult PlaceOrder(const core::OrderPlaceRequest& request) noexcept {
    ++place_calls;
    last_place_local_order_id = request.local_order_id;
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk,
            .send_local_ns = benchmarking::RealtimeNowNs()};
  }

  SendResult CancelOrder(const core::OrderCancelRequest&) noexcept {
    benchmark::ClobberMemory();
    return {.status = SendStatus::kOk, .send_local_ns = 0};
  }
};

void BM_OrderManagerPlaceSynthetic(benchmark::State& state) {
  FakeOrderSession session;
  core::OrderManager<FakeOrderSession> order_manager(
      session, kLatencyIterations + 8, kStrategyId);
  const core::OrderPlaceRequest request = SyntheticOrderRequest();

  RunManualLatencyBenchmark(
      state,
      [&] {
        const core::OrderPlaceResult placed = order_manager.PlaceOrder(request);
        if (placed.status != core::OrderPlaceStatus::kOk) {
          state.SkipWithError("order manager place failed");
          return;
        }
        std::uint64_t local_order_id = placed.local_order_id;
        benchmark::DoNotOptimize(local_order_id);
        benchmark::DoNotOptimize(session.last_place_local_order_id);
      },
      "orders", state.iterations());
  benchmark::DoNotOptimize(session.place_calls);
}

class GatewayClientBenchmarkState {
 public:
  explicit GatewayClientBenchmarkState(std::uint64_t route_table_capacity) {
    shm_name_ = std::string{"aquila_submit_breakdown_"} +
                std::to_string(static_cast<unsigned long long>(::getpid())) +
                "_" + std::to_string(++next_instance_id_);
    core::OrderGatewayShmConfig shm_config{
        .shm_name = shm_name_,
        .create = true,
        .remove_existing = true,
        .route_count = kFanout,
        .command_queue_capacity = kGatewayQueueCapacity,
        .event_queue_capacity = 64,
        .startup_ready_timeout_s = 1,
    };
    auto manager_result = core::OrderGatewayShmManager::Create(shm_config);
    if (!manager_result.ok) {
      error_ = std::move(manager_result.error);
      return;
    }
    for (std::uint16_t route = 0; route < kFanout; ++route) {
      core::StoreOrderGatewayRouteState(manager_result.value.header(), route,
                                        core::OrderGatewayRouteState::kReady);
    }
    auto client_result = core::OrderGatewayClient::Attach(
        std::move(manager_result.value),
        core::OrderGatewayClientOptions{
            .route_table_capacity = route_table_capacity,
            .max_events_per_poll_per_route = 64,
        });
    if (!client_result.ok) {
      error_ = std::move(client_result.error);
      return;
    }
    client_ = std::move(client_result.value);
    if (!client_.Start()) {
      error_ = "order gateway client start failed";
      return;
    }
    const std::string normalized_name = "/" + shm_name_;
    ::shm_unlink(normalized_name.c_str());
    ok_ = true;
  }

  [[nodiscard]] bool ok() const noexcept {
    return ok_;
  }
  [[nodiscard]] const std::string& error() const noexcept {
    return error_;
  }
  [[nodiscard]] core::OrderGatewayClient& client() noexcept {
    return client_;
  }

 private:
  static inline std::uint64_t next_instance_id_{0};

  std::string shm_name_;
  std::string error_;
  core::OrderGatewayClient client_;
  bool ok_{false};
};

struct FanoutSubmitTiming {
  std::array<std::int64_t, kFanout> route_send_ns{};
  std::uint64_t command_seq_accumulator{0};
  std::uint64_t commands_enqueued{0};
};

struct FanoutSubmitSamples {
  std::vector<std::uint64_t> total_ns;
  std::vector<std::uint64_t> first_route_ns;
  std::vector<std::uint64_t> last_route_ns;
  std::vector<std::uint64_t> route_spread_ns;

  void Reserve(std::size_t size) {
    total_ns.reserve(size);
    first_route_ns.reserve(size);
    last_route_ns.reserve(size);
    route_spread_ns.reserve(size);
  }

  void Push(std::uint64_t elapsed_ns, std::int64_t route_start_ns,
            const FanoutSubmitTiming& timing) {
    total_ns.push_back(elapsed_ns);
    first_route_ns.push_back(DeltaNs(route_start_ns, timing.route_send_ns[0]));
    last_route_ns.push_back(
        DeltaNs(route_start_ns, timing.route_send_ns[kFanout - 1]));
    route_spread_ns.push_back(
        DeltaNs(timing.route_send_ns[0], timing.route_send_ns[kFanout - 1]));
  }

  void SetCounters(benchmark::State& state) {
    websocket::benchmarking::SetLatencyCounters(state, std::move(total_ns),
                                                "parents", state.iterations());
    SetPrefixedLatencyCounters(state, "first_route", first_route_ns);
    SetPrefixedLatencyCounters(state, "last_route", last_route_ns);
    SetPrefixedLatencyCounters(state, "route_spread", route_spread_ns);
    state.counters["fanout_routes"] = static_cast<double>(kFanout);
  }
};

enum class FanoutBatchModelStatus : std::uint8_t {
  kOk,
  kNotRunning,
  kInvalidRoute,
  kRouteNotReady,
  kCommandQueueFull,
};

struct FanoutBatchModelResult {
  FanoutBatchModelStatus status{FanoutBatchModelStatus::kNotRunning};
  FanoutSubmitTiming timing{};
};

class FanoutBatchModelGatewayState {
 public:
  FanoutBatchModelGatewayState() {
    shm_name_ = std::string{"aquila_submit_batch_model_"} +
                std::to_string(static_cast<unsigned long long>(::getpid())) +
                "_" + std::to_string(++next_instance_id_);
    core::OrderGatewayShmConfig shm_config{
        .shm_name = shm_name_,
        .create = true,
        .remove_existing = true,
        .route_count = kFanout,
        .command_queue_capacity = kGatewayQueueCapacity,
        .event_queue_capacity = 64,
        .startup_ready_timeout_s = 1,
    };
    auto manager_result = core::OrderGatewayShmManager::Create(shm_config);
    if (!manager_result.ok) {
      error_ = std::move(manager_result.error);
      return;
    }
    manager_ = std::move(manager_result.value);
    for (std::uint16_t route = 0; route < kFanout; ++route) {
      core::StoreOrderGatewayRouteState(manager_.header(), route,
                                        core::OrderGatewayRouteState::kReady);
      command_queues_[route] = manager_.CommandQueue(route);
      route_ready_[route] = true;
    }
    const std::string normalized_name = "/" + shm_name_;
    ::shm_unlink(normalized_name.c_str());
    ok_ = true;
  }

  [[nodiscard]] bool ok() const noexcept {
    return ok_;
  }
  [[nodiscard]] const std::string& error() const noexcept {
    return error_;
  }

  [[nodiscard]] FanoutBatchModelResult EnqueueFanoutBatch4(
      const std::array<core::OrderPlaceRequest, kFanout>& orders) noexcept {
    // Benchmark-only lower-bound model; this is not a production batch API.
    FanoutBatchModelResult result;
    if (!ok_ || !running_) {
      result.status = FanoutBatchModelStatus::kNotRunning;
      return result;
    }
    RefreshRouteStatesOnce();

    for (const core::OrderPlaceRequest& request : orders) {
      if (request.gateway_route_id >= kFanout) {
        result.status = FanoutBatchModelStatus::kInvalidRoute;
        return result;
      }
      if (!route_ready_[request.gateway_route_id]) {
        result.status = FanoutBatchModelStatus::kRouteNotReady;
        return result;
      }
    }

    for (const core::OrderPlaceRequest& request : orders) {
      const std::uint16_t route = request.gateway_route_id;
      core::OrderGatewayCommand command{};
      command.command_seq = ++command_seq_;
      command.owner_enqueue_ns =
          static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
      command.payload.place = request;
      command.kind = core::OrderGatewayCommandKind::kPlace;
      if (!command_queues_[route].TryPush(command)) {
        result.status = FanoutBatchModelStatus::kCommandQueueFull;
        return result;
      }
      ++commands_enqueued_;
      result.timing.route_send_ns[route] = command.owner_enqueue_ns;
      result.timing.command_seq_accumulator += command.command_seq;
      ++result.timing.commands_enqueued;
    }

    result.status = FanoutBatchModelStatus::kOk;
    return result;
  }

  [[nodiscard]] std::uint64_t commands_enqueued() const noexcept {
    return commands_enqueued_;
  }

 private:
  void RefreshRouteStatesOnce() noexcept {
    for (std::uint16_t route = 0; route < kFanout; ++route) {
      route_ready_[route] =
          core::LoadOrderGatewayRouteState(manager_.header(), route) ==
          core::OrderGatewayRouteState::kReady;
    }
  }

  static inline std::uint64_t next_instance_id_{0};

  std::string shm_name_;
  std::string error_;
  core::OrderGatewayShmManager manager_;
  std::array<core::OrderGatewayCommandQueue, kFanout> command_queues_{};
  std::array<bool, kFanout> route_ready_{};
  std::uint64_t command_seq_{0};
  std::uint64_t commands_enqueued_{0};
  bool running_{true};
  bool ok_{false};
};

void BM_OrderGatewayClientPlaceCommandSynthetic(benchmark::State& state) {
  GatewayClientBenchmarkState gateway{kLatencyIterations + 16};
  if (!gateway.ok()) {
    state.SkipWithError(gateway.error().c_str());
    return;
  }
  std::uint64_t local_order_id = 1;

  RunManualLatencyBenchmark(
      state,
      [&] {
        const core::OrderGatewaySendResult sent = gateway.client().PlaceOrder(
            SyntheticOrderRequest(local_order_id++, 1, 0));
        if (sent.status != core::OrderGatewaySendStatus::kOk) {
          state.SkipWithError("order gateway client place failed");
          return;
        }
        std::uint64_t command_seq = sent.command_seq;
        benchmark::DoNotOptimize(command_seq);
      },
      "commands", state.iterations());
}

void BM_OrderGatewayClientPlaceFanout4Synthetic(benchmark::State& state) {
  GatewayClientBenchmarkState gateway{kLatencyIterations * kFanout + 16};
  if (!gateway.ok()) {
    state.SkipWithError(gateway.error().c_str());
    return;
  }
  std::uint64_t local_order_id = 1;
  std::uint64_t group_id = 1;

  RunManualLatencyBenchmark(
      state,
      [&] {
        for (std::uint16_t route = 0; route < kFanout; ++route) {
          const core::OrderGatewaySendResult sent = gateway.client().PlaceOrder(
              SyntheticOrderRequest(local_order_id++, group_id, route));
          if (sent.status != core::OrderGatewaySendStatus::kOk) {
            state.SkipWithError("order gateway client fanout place failed");
            return;
          }
          std::uint64_t command_seq = sent.command_seq;
          benchmark::DoNotOptimize(command_seq);
        }
        ++group_id;
      },
      "groups", state.iterations());
}

void BM_OrderGatewayFanoutCurrentPlaceOrder4Routes(benchmark::State& state) {
  GatewayClientBenchmarkState gateway{kLatencyIterations * kFanout + 16};
  if (!gateway.ok()) {
    state.SkipWithError(gateway.error().c_str());
    return;
  }
  FanoutSubmitSamples samples;
  samples.Reserve(kLatencyIterations);
  std::uint64_t local_order_id = 1;
  std::uint64_t group_id = 1;

  for (auto _ : state) {
    const std::array<core::OrderPlaceRequest, kFanout> orders =
        SyntheticFanoutOrders(local_order_id, group_id);
    FanoutSubmitTiming timing;
    const std::int64_t route_start_ns =
        static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    for (std::uint16_t route = 0; route < kFanout; ++route) {
      const core::OrderGatewaySendResult sent =
          gateway.client().PlaceOrder(orders[route]);
      if (sent.status != core::OrderGatewaySendStatus::kOk) {
        state.SkipWithError("current PlaceOrder fanout failed");
        return;
      }
      timing.route_send_ns[route] = sent.send_local_ns;
      timing.command_seq_accumulator += sent.command_seq;
      ++timing.commands_enqueued;
    }
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples.Push(elapsed_ns, route_start_ns, timing);
    local_order_id += kFanout;
    ++group_id;
    benchmark::DoNotOptimize(timing.command_seq_accumulator);
    benchmark::DoNotOptimize(timing.commands_enqueued);
  }

  samples.SetCounters(state);
  state.counters["benchmark_only_batch_model"] = 0.0;
  state.counters["commands_enqueued"] =
      static_cast<double>(gateway.client().stats().commands_enqueued);
}

void BM_OrderGatewayFanoutBatchModel4Routes(benchmark::State& state) {
  FanoutBatchModelGatewayState gateway;
  if (!gateway.ok()) {
    state.SkipWithError(gateway.error().c_str());
    return;
  }
  FanoutSubmitSamples samples;
  samples.Reserve(kLatencyIterations);
  std::uint64_t local_order_id = 1;
  std::uint64_t group_id = 1;

  for (auto _ : state) {
    const std::array<core::OrderPlaceRequest, kFanout> orders =
        SyntheticFanoutOrders(local_order_id, group_id);
    const std::int64_t route_start_ns =
        static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    FanoutBatchModelResult result = gateway.EnqueueFanoutBatch4(orders);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    if (result.status != FanoutBatchModelStatus::kOk ||
        result.timing.commands_enqueued != kFanout) {
      state.SkipWithError("benchmark-only batch model fanout failed");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples.Push(elapsed_ns, route_start_ns, result.timing);
    local_order_id += kFanout;
    ++group_id;
    benchmark::DoNotOptimize(result.timing.command_seq_accumulator);
    benchmark::DoNotOptimize(result.timing.commands_enqueued);
  }

  samples.SetCounters(state);
  state.counters["benchmark_only_batch_model"] = 1.0;
  state.counters["commands_enqueued"] =
      static_cast<double>(gateway.commands_enqueued());
}

template <typename OrderSessionT>
using E2EDirectRuntime = core::TradingRuntime<Strategy, OrderSessionT,
                                              market_data::RealtimeDataReader<>,
                                              core::TradingRuntimeDiagnostics>;

template <typename OrderSessionT>
void RunLeadLagDirectPreparedOrderToWriteLatency(benchmark::State& state,
                                                 Exchange lag_exchange) {
  EnsureE2ELoggingInitialized();
  StrategyLogHookScope hooks;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);

  for (auto _ : state) {
    state.PauseTiming();
    SubmitTrace trace;
    bool session_ready = false;
    auto runtime_result = E2EDirectRuntime<OrderSessionT>::CreateForTest(
        RuntimeConfig(),
        [&session_ready] {
          OrderSessionT session;
          session_ready = session.Ready() && session.WarmPlaceOrder();
          return session;
        },
        BenchmarkLeadLagConfig(lag_exchange));
    if (!runtime_result.ok || !session_ready) {
      state.ResumeTiming();
      state.SkipWithError(runtime_result.ok ? "order session login setup failed"
                                            : runtime_result.error.c_str());
      return;
    }

    E2EDirectRuntime<OrderSessionT>& runtime = *runtime_result.value;
    const std::int64_t trigger_event_ns =
        SeedBeforeOpenSignal(runtime, kSymbolId, lag_exchange);
    const BookTicker trigger_ticker =
        OpenLongTriggerTicker(trigger_event_ns, kSymbolId);
    trace.Reset();
    E2ECountingOrderTransport::ResetStats();
    active_submit_trace = &trace;
    state.ResumeTiming();

    runtime.HandleBookTickerForTest(trigger_ticker);

    state.PauseTiming();
    active_submit_trace = nullptr;
    const std::int64_t start_ns = RouteStageTimeNs(
        trace, StrategySubmitStageForTest::kBeforeAcquireRiskSlot, 0);
    const E2ETransportStats transport_stats =
        E2ECountingOrderTransport::stats();
    const std::int64_t end_ns = transport_stats.last_write_ns;
    if (start_ns == 0 || end_ns <= start_ns ||
        transport_stats.invalid_writes != 0 ||
        transport_stats.write_calls != 1) {
      const std::string error = fmt::format(
          "direct prepared-order to write path failed: start_ns={} "
          "end_ns={} writes={} invalid_writes={} place_calls={} "
          "submitted_calls={}",
          start_ns, end_ns, transport_stats.write_calls,
          transport_stats.invalid_writes, trace.place_calls,
          trace.submitted_calls);
      state.ResumeTiming();
      state.SkipWithError(error.c_str());
      return;
    }

    const std::uint64_t elapsed_ns = DeltaNs(start_ns, end_ns);
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    std::uint64_t bytes_sent = transport_stats.bytes_sent;
    benchmark::DoNotOptimize(bytes_sent);
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "orders", state.iterations());
}

void BM_LeadLagGateDirectPreparedOrderToWriteLatency(benchmark::State& state) {
  RunLeadLagDirectPreparedOrderToWriteLatency<GateE2EOrderSession>(
      state, Exchange::kGate);
}

void BM_LeadLagBitgetDirectPreparedOrderToWriteLatency(
    benchmark::State& state) {
  RunLeadLagDirectPreparedOrderToWriteLatency<BitgetE2EOrderSession>(
      state, Exchange::kBitget);
}

template <typename OrderSessionT>
void RunLeadLagShmPreparedOrderToWriteLatency(benchmark::State& state,
                                              Exchange lag_exchange) {
  EnsureE2ELoggingInitialized();
  StrategyLogHookScope hooks;
  std::vector<std::uint64_t> samples_ns;
  std::vector<std::uint64_t> strategy_samples_ns;
  std::vector<std::uint64_t> handoff_samples_ns;
  std::vector<std::uint64_t> order_session_samples_ns;
  samples_ns.reserve(kLatencyIterations);
  strategy_samples_ns.reserve(kLatencyIterations);
  handoff_samples_ns.reserve(kLatencyIterations);
  order_session_samples_ns.reserve(kLatencyIterations);
  static std::uint64_t next_instance_id = 0;

  for (auto _ : state) {
    state.PauseTiming();
    SubmitTrace trace;
    const std::string shm_name = fmt::format(
        "/aquila_order_request_e2e_{}_{}",
        static_cast<unsigned long long>(::getpid()), ++next_instance_id);
    E2EShmCleanup cleanup(shm_name);
    const core::OrderGatewayShmConfig shm_config{
        .shm_name = shm_name,
        .create = true,
        .remove_existing = true,
        .route_count = 1,
        .command_queue_capacity = 8,
        .event_queue_capacity = 8,
        .startup_ready_timeout_s = 1,
    };
    auto manager_result = core::OrderGatewayShmManager::Create(shm_config);
    if (!manager_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(manager_result.error.c_str());
      return;
    }
    core::OrderGatewayShmManager& manager = manager_result.value;
    core::StoreOrderGatewayRouteState(manager.header(), 0,
                                      core::OrderGatewayRouteState::kReady);

    auto client_result =
        core::OrderGatewayClient::Open(core::OrderGatewayClientConfig{
            .shm_name = shm_name,
            .route_count = 1,
            .command_queue_capacity = 8,
            .event_queue_capacity = 8,
            .startup_ready_timeout_s = 1,
            .options =
                core::OrderGatewayClientOptions{
                    .route_table_capacity = kOrderCapacity,
                    .max_events_per_poll_per_route = 8,
                },
        });
    if (!client_result.ok || !client_result.value.Start()) {
      state.ResumeTiming();
      state.SkipWithError(client_result.ok ? "order gateway client start failed"
                                           : client_result.error.c_str());
      return;
    }

    OrderSessionT order_session;
    if (!order_session.Ready() || !order_session.WarmPlaceOrder()) {
      state.ResumeTiming();
      state.SkipWithError("order session warmup setup failed");
      return;
    }
    using Traits = E2EExchangeTraits<OrderSessionT>;
    typename Traits::Publisher publisher(0, manager.EventQueue(0),
                                         &manager.header());
    typename Traits::Worker worker(0, manager.CommandQueue(0), order_session,
                                   publisher);

    auto runtime_result =
        E2EDirectRuntime<core::OrderGatewayClient>::CreateForTest(
            RuntimeConfig(),
            [client = std::move(client_result.value)]() mutable {
              return std::move(client);
            },
            BenchmarkLeadLagConfig(lag_exchange));
    if (!runtime_result.ok) {
      state.ResumeTiming();
      state.SkipWithError(runtime_result.error.c_str());
      return;
    }
    E2EDirectRuntime<core::OrderGatewayClient>& runtime = *runtime_result.value;
    const std::int64_t trigger_event_ns =
        SeedBeforeOpenSignal(runtime, kSymbolId, lag_exchange);
    const BookTicker trigger_ticker =
        OpenLongTriggerTicker(trigger_event_ns, kSymbolId);
    trace.Reset();
    E2ECountingOrderTransport::ResetStats();
    e2e_order_session_place_entry_ns = 0;
    active_submit_trace = &trace;
    state.ResumeTiming();

    runtime.HandleBookTickerForTest(trigger_ticker);
    const bool dispatched = worker.PollOnce();

    state.PauseTiming();
    active_submit_trace = nullptr;
    const std::int64_t start_ns = RouteStageTimeNs(
        trace, StrategySubmitStageForTest::kBeforeAcquireRiskSlot, 0);
    const std::int64_t before_place_ns = RouteStageTimeNs(
        trace, StrategySubmitStageForTest::kBeforePlaceOrder, 0);
    const std::int64_t session_entry_ns = e2e_order_session_place_entry_ns;
    const E2ETransportStats transport_stats =
        E2ECountingOrderTransport::stats();
    const std::int64_t end_ns = transport_stats.last_write_ns;
    if (!dispatched || start_ns == 0 || before_place_ns <= start_ns ||
        session_entry_ns <= before_place_ns || end_ns <= session_entry_ns ||
        transport_stats.invalid_writes != 0 ||
        transport_stats.write_calls != 1) {
      const std::string error = fmt::format(
          "SHM prepared-order to write path failed: dispatched={} "
          "start_ns={} before_place_ns={} session_entry_ns={} end_ns={} "
          "writes={} invalid_writes={} submitted_calls={}",
          dispatched, start_ns, before_place_ns, session_entry_ns, end_ns,
          transport_stats.write_calls, transport_stats.invalid_writes,
          trace.submitted_calls);
      state.ResumeTiming();
      state.SkipWithError(error.c_str());
      return;
    }

    const std::uint64_t elapsed_ns = DeltaNs(start_ns, end_ns);
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    strategy_samples_ns.push_back(DeltaNs(start_ns, before_place_ns));
    handoff_samples_ns.push_back(DeltaNs(before_place_ns, session_entry_ns));
    order_session_samples_ns.push_back(DeltaNs(session_entry_ns, end_ns));
    std::uint64_t bytes_sent = transport_stats.bytes_sent;
    benchmark::DoNotOptimize(bytes_sent);
    state.ResumeTiming();
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "orders", state.iterations());
  SetPrefixedLatencyCounters(state, "strategy", strategy_samples_ns);
  SetPrefixedLatencyCounters(state, "shm_handoff", handoff_samples_ns);
  SetPrefixedLatencyCounters(state, "order_session", order_session_samples_ns);
}

void BM_LeadLagGateShmPreparedOrderToWriteLatency(benchmark::State& state) {
  RunLeadLagShmPreparedOrderToWriteLatency<GateE2EOrderSession>(
      state, Exchange::kGate);
}

void BM_LeadLagBitgetShmPreparedOrderToWriteLatency(benchmark::State& state) {
  RunLeadLagShmPreparedOrderToWriteLatency<BitgetE2EOrderSession>(
      state, Exchange::kBitget);
}

BENCHMARK(BM_LeadLagSubmitPathBreakdownSyntheticFanout4)
    ->Iterations(kSubmitBreakdownIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagSubmitPathBreakdownOrderGatewaySyntheticFanout4)
    ->Iterations(kSubmitBreakdownIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagSubmitPathBreakdownOrderGatewayLiveLike30RiskOff)
    ->Iterations(kSubmitBreakdownIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagSubmitPathBreakdownOrderGatewayLiveLike30RiskOn)
    ->Iterations(kSubmitBreakdownIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagSubmitPathBreakdownOrderGatewayLiveLike30RiskOnPrefill20)
    ->Iterations(kSubmitBreakdownIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagSubmitPathBreakdownOrderGatewayBitget46Fanout1Churn)
    ->Arg(1)
    ->Arg(8)
    ->Arg(64)
    ->Iterations(kColdSubmitBreakdownIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagSubmitPathBreakdownOrderGatewayBitget46Fanout1Warm)
    ->Arg(1)
    ->Arg(8)
    ->Arg(64)
    ->Iterations(kColdSubmitBreakdownIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagSubmitPathBreakdownOrderGatewayBitget46Fanout1EndpointOnly)
    ->Arg(64)
    ->Iterations(kColdSubmitBreakdownIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagSubmitPathBreakdownOrderGatewayActualConfigRiskOnBas)
    ->Iterations(kSubmitBreakdownIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagCurrentGlobalRiskTotalsActualConfig)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BM_LogStrategySignalTriggeredSynthetic)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LogStrategyOrderIntentSynthetic)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LogStrategyOrderSubmittedSynthetic)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderDecimalPreparePriceQuantityAndTextSynthetic)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderManagerPlaceSynthetic)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderGatewayClientPlaceCommandSynthetic)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderGatewayClientPlaceFanout4Synthetic)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderGatewayFanoutCurrentPlaceOrder4Routes)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderGatewayFanoutBatchModel4Routes)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagGateDirectPreparedOrderToWriteLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagBitgetDirectPreparedOrderToWriteLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagGateShmPreparedOrderToWriteLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LeadLagBitgetShmPreparedOrderToWriteLatency)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila::strategy::leadlag
