#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
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
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/common/types.h"
#include "core/trading/order_gateway_shm.h"
#include "core/trading/order_manager.h"
#include "core/trading/strategy_context.h"
#include "core/websocket/message_view.h"
#include "exchange/bitget/trading/operation_response_parser.h"
#include "exchange/bitget/trading/order_gateway_worker.h"
#include "exchange/bitget/trading/order_request_encoder.h"
#include "exchange/bitget/trading/order_session.h"
#include <simdjson.h>

namespace aquila::bitget {
namespace {

constexpr std::string_view kPlaceAck =
    R"({"event":"trade","id":"72057594037927945","category":"usdt-futures","topic":"place-order","args":[{"symbol":"BTCUSDT","orderId":"123456789","clientOid":"a-42","cTime":"1750034397008"}],"code":"0","msg":"success","connId":"connection-1","ts":"1750034397076"})";

constexpr std::string_view kPlaceError =
    R"({"event":"error","id":"72057594037927945","topic":"place-order","code":"40010","msg":"Request timed out","ts":"1750034397076"})";

constexpr std::string_view kLoginSuccess =
    R"({"event":"login","code":"0","msg":""})";

constexpr std::size_t kOrderSendLatencyIterations = 4096;

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

  std::string shm_name;
};

struct CountingHandler {
  void OnOrderResponse(const OrderResponse&) noexcept {
    ++responses;
  }

  std::uint64_t responses{0};
};

struct CountingTransportStats {
  std::uint64_t invalid_writes{0};
  std::uint64_t write_calls{0};
  std::uint64_t bytes_sent{0};
};

class CountingOrderTransport {
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

  [[nodiscard]] static CountingTransportStats stats() noexcept {
    return stats_;
  }

 private:
  bool wants_read_{false};
  bool wants_write_{false};

  inline static CountingTransportStats stats_{};
};

struct CountingOrderWebSocketPolicy : websocket::DefaultWebSocketOptions {
  using TransportSocket = CountingOrderTransport;
};

[[nodiscard]] websocket::MessageView TextView(
    std::string_view payload) noexcept {
  return {
      .kind = websocket::PayloadKind::kText,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 1,
      .fin = true,
  };
}

struct PaddedTextPayload {
  [[nodiscard]] websocket::MessageView View() const noexcept {
    return {
        .kind = websocket::PayloadKind::kText,
        .payload =
            std::as_bytes(std::span<const char>(storage.data(), payload_size)),
        .sequence = 1,
        .fin = true,
        .readable_tail_bytes = simdjson::SIMDJSON_PADDING,
    };
  }

  std::vector<char> storage{};
  std::size_t payload_size{0};
};

[[nodiscard]] PaddedTextPayload MakePlaceAckPayload(
    std::uint64_t request_sequence, std::uint64_t local_order_id) {
  const std::string payload = fmt::format(
      R"({{"event":"trade","id":"{}","category":"usdt-futures","topic":"place-order","args":[{{"symbol":"BTCUSDT","orderId":"123456789","clientOid":"a-{}","cTime":"1750034397008"}}],"code":"0","msg":"success","connId":"connection-1","ts":"1750034397076"}})",
      RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, request_sequence),
      local_order_id);
  PaddedTextPayload result{
      .storage = std::vector<char>(payload.size() + simdjson::SIMDJSON_PADDING),
      .payload_size = payload.size(),
  };
  std::memcpy(result.storage.data(), payload.data(), payload.size());
  return result;
}

[[nodiscard]] websocket::ConnectionConfig MakeOrderSessionConfig() {
  websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.port = "80";
  config.target = "/v3/ws/private";
  config.enable_tls = false;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 4096;
  return config;
}

[[nodiscard]] core::OrderGatewayShmConfig MakeOrderGatewayShmConfig() {
  return core::OrderGatewayShmConfig{
      .shm_name =
          fmt::format("/aquila_bitget_order_gateway_bench_{}", ::getpid()),
      .create = true,
      .remove_existing = true,
      .route_count = 1,
      .command_queue_capacity =
          static_cast<std::uint32_t>(kOrderSendLatencyIterations + 2),
      .event_queue_capacity =
          static_cast<std::uint32_t>(kOrderSendLatencyIterations + 2),
      .startup_ready_timeout_s = 30,
  };
}

[[nodiscard]] core::OrderPlaceRequest MakeStrategyOrder(
    std::uint64_t local_order_id) noexcept {
  core::OrderPlaceRequest request{
      .local_order_id = local_order_id,
      .price = 100000.0,
      .quantity = 0.001,
      .symbol_id = 7,
      .exchange = Exchange::kBitget,
      .side = OrderSide::kBuy,
      .order_type = OrderType::kLimit,
      .time_in_force = TimeInForce::kImmediateOrCancel,
      .price_decimal_places = 1,
      .quantity_decimal_places = 3,
      .reduce_only = false,
  };
  core::SetOrderSymbol(&request, "BTCUSDT");
  return request;
}

[[nodiscard]] core::OrderGatewayCommand MakePlaceCommand(
    std::uint64_t local_order_id) noexcept {
  core::OrderGatewayCommand command{};
  command.kind = core::OrderGatewayCommandKind::kPlace;
  command.command_seq = local_order_id;
  command.payload.place = MakeStrategyOrder(local_order_id);
  command.payload.place.group_id = 1;
  command.payload.place.gateway_route_id = 0;
  return command;
}

[[nodiscard]] core::OrderPlaceRequest MakeLimitRequest() noexcept {
  core::OrderPlaceRequest request{
      .price = 100000.0,
      .quantity = 0.001,
      .symbol_id = 7,
      .exchange = Exchange::kBitget,
      .side = OrderSide::kBuy,
      .order_type = OrderType::kLimit,
      .time_in_force = TimeInForce::kImmediateOrCancel,
      .price_decimal_places = 1,
      .quantity_decimal_places = 3,
      .reduce_only = false,
  };
  core::SetOrderSymbol(&request, "BTCUSDT");
  return request;
}

template <typename Session>
[[nodiscard]] bool ActivateLoginOnly(Session& session) noexcept {
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  return session.Handle(TextView(kLoginSuccess)) ==
             websocket::DeliveryResult::kAccepted &&
         session.Ready();
}

template <typename Func>
void RunOrderSendLatencyBenchmark(benchmark::State& state, Func&& func) {
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kOrderSendLatencyIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const auto result = func();
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    if (result.status != OrderSendStatus::kOk) {
      state.SkipWithError("Bitget order send failed");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    std::uint64_t encoded_request_id = result.encoded_request_id;
    benchmark::DoNotOptimize(encoded_request_id);
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "orders", state.iterations());
}

void BenchmarkEncodePlace(benchmark::State& state) {
  const core::OrderPlaceRequest request = MakeStrategyOrder(42);
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  for (auto _ : state) {
    const EncodedTextRequest encoded =
        EncodePlaceOrderRequest(request, 72057594037927945ULL, buffer);
    benchmark::DoNotOptimize(encoded.text.data());
    benchmark::DoNotOptimize(encoded.text.size());
  }
}

void BenchmarkEncodeCancel(benchmark::State& state) {
  const core::OrderCancelRequest request{.local_order_id = 42};
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  for (auto _ : state) {
    const EncodedTextRequest encoded = EncodeCancelOrderRequest(
        request, 123456789, 144115188075855881ULL, buffer);
    benchmark::DoNotOptimize(encoded.text.data());
    benchmark::DoNotOptimize(encoded.text.size());
  }
}

template <std::size_t N>
void BenchmarkParse(benchmark::State& state, std::string_view payload) {
  std::array<char, N> scratch{};
  std::copy(payload.begin(), payload.end(), scratch.begin());
  simdjson::ondemand::parser parser;
  for (auto _ : state) {
    OperationResponse response =
        ParseOperationResponse(std::string_view(scratch.data(), payload.size()),
                               simdjson::SIMDJSON_PADDING, parser);
    benchmark::DoNotOptimize(response.kind);
    benchmark::DoNotOptimize(response.exchange_order_id);
  }
}

void BenchmarkParsePlaceAck(benchmark::State& state) {
  BenchmarkParse<kPlaceAck.size() + simdjson::SIMDJSON_PADDING>(state,
                                                                kPlaceAck);
}

void BenchmarkParsePlaceError(benchmark::State& state) {
  BenchmarkParse<kPlaceError.size() + simdjson::SIMDJSON_PADDING>(state,
                                                                  kPlaceError);
}

void BenchmarkOrderSessionPlaceOrderToCountingTransport(
    benchmark::State& state) {
  CountingOrderTransport::ResetStats();
  CountingHandler handler;
  using BenchOrderSession =
      OrderSession<CountingHandler, CountingOrderWebSocketPolicy>;
  BenchOrderSession session(
      MakeOrderSessionConfig(),
      LoginCredentials{
          .api_key = "key", .api_secret = "secret", .passphrase = "phrase"},
      handler, kOrderSendLatencyIterations + 2,
      kOrderSendLatencyIterations + 2);
  if (!ActivateLoginOnly(session)) {
    state.SkipWithError("Bitget order session login setup failed");
    return;
  }
  CountingOrderTransport::ResetStats();
  std::uint64_t local_order_id = 1;

  RunOrderSendLatencyBenchmark(state, [&session, &local_order_id] {
    const core::OrderPlaceRequest order = MakeStrategyOrder(local_order_id++);
    return session.PlaceOrder(order);
  });

  const CountingTransportStats stats = CountingOrderTransport::stats();
  if (stats.invalid_writes != 0 ||
      stats.write_calls != static_cast<std::uint64_t>(state.iterations())) {
    state.SkipWithError("Bitget order session write count mismatch");
  }
  state.counters["bytes_sent"] = static_cast<double>(stats.bytes_sent);
}

void BenchmarkOrderSessionPlaceAckToCountingHandler(benchmark::State& state) {
  std::vector<PaddedTextPayload> payloads;
  payloads.reserve(kOrderSendLatencyIterations);
  for (std::uint64_t i = 0; i < kOrderSendLatencyIterations; ++i) {
    payloads.push_back(MakePlaceAckPayload(i + 1, i + 1));
  }

  CountingOrderTransport::ResetStats();
  CountingHandler handler;
  using BenchOrderSession =
      OrderSession<CountingHandler, CountingOrderWebSocketPolicy>;
  BenchOrderSession session(
      MakeOrderSessionConfig(),
      LoginCredentials{
          .api_key = "key", .api_secret = "secret", .passphrase = "phrase"},
      handler, kOrderSendLatencyIterations + 2,
      kOrderSendLatencyIterations + 2);
  if (!ActivateLoginOnly(session)) {
    state.SkipWithError("Bitget order session login setup failed");
    return;
  }
  CountingOrderTransport::ResetStats();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kOrderSendLatencyIterations);
  std::uint64_t local_order_id = 1;

  for (auto _ : state) {
    const OrderSendResult send_result =
        session.PlaceOrder(MakeStrategyOrder(local_order_id));
    if (send_result.status != OrderSendStatus::kOk ||
        send_result.request_sequence != local_order_id) {
      state.SkipWithError("Bitget order session place setup failed");
      return;
    }
    const websocket::MessageView view = payloads[local_order_id - 1].View();
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const websocket::DeliveryResult delivery = session.Handle(view);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    if (delivery != websocket::DeliveryResult::kAccepted) {
      state.SkipWithError("Bitget order session ACK handling failed");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    ++local_order_id;
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "responses", handler.responses);
  const CountingTransportStats stats = CountingOrderTransport::stats();
  if (stats.invalid_writes != 0 ||
      stats.write_calls != static_cast<std::uint64_t>(state.iterations()) ||
      handler.responses != static_cast<std::uint64_t>(state.iterations())) {
    state.SkipWithError("Bitget order session ACK count mismatch");
  }
}

void BenchmarkStrategyContextPlaceLimitOrderToCountingTransport(
    benchmark::State& state) {
  CountingOrderTransport::ResetStats();
  CountingHandler handler;
  using BenchOrderSession =
      OrderSession<CountingHandler, CountingOrderWebSocketPolicy>;
  BenchOrderSession session(
      MakeOrderSessionConfig(),
      LoginCredentials{
          .api_key = "key", .api_secret = "secret", .passphrase = "phrase"},
      handler, kOrderSendLatencyIterations + 2,
      kOrderSendLatencyIterations + 2);
  if (!ActivateLoginOnly(session)) {
    state.SkipWithError("Bitget order session login setup failed");
    return;
  }
  core::OrderManager<BenchOrderSession> order_manager(
      session, kOrderSendLatencyIterations + 2);
  core::StrategyContext<BenchOrderSession> context(order_manager);
  CountingOrderTransport::ResetStats();

  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kOrderSendLatencyIterations);
  for (auto _ : state) {
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const core::OrderPlaceResult result =
        context.PlaceLimitOrder(MakeLimitRequest());
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    if (result.status != core::OrderPlaceStatus::kOk) {
      state.SkipWithError("Bitget strategy order send failed");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
    std::uint64_t local_order_id = result.local_order_id;
    benchmark::DoNotOptimize(local_order_id);
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "orders", state.iterations());
  const CountingTransportStats stats = CountingOrderTransport::stats();
  if (stats.invalid_writes != 0 ||
      stats.write_calls != static_cast<std::uint64_t>(state.iterations())) {
    state.SkipWithError("Bitget strategy order write count mismatch");
  }
  state.counters["bytes_sent"] = static_cast<double>(stats.bytes_sent);
}

void BenchmarkGatewayWorkerPlaceOrderToCountingTransport(
    benchmark::State& state) {
  const core::OrderGatewayShmConfig shm_config = MakeOrderGatewayShmConfig();
  ShmCleanup cleanup(shm_config.shm_name);
  auto shm_result = core::OrderGatewayShmManager::Create(shm_config);
  if (!shm_result.ok) {
    state.SkipWithError(shm_result.error.c_str());
    return;
  }
  core::OrderGatewayShmManager& shm = shm_result.value;
  for (std::uint64_t i = 0; i < kOrderSendLatencyIterations; ++i) {
    if (!shm.CommandQueue(0).TryPush(MakePlaceCommand(i + 1))) {
      state.SkipWithError("failed to prefill Bitget gateway command queue");
      return;
    }
  }

  CountingOrderTransport::ResetStats();
  CountingHandler handler;
  using BenchOrderSession =
      OrderSession<CountingHandler, CountingOrderWebSocketPolicy>;
  BenchOrderSession session(
      MakeOrderSessionConfig(),
      LoginCredentials{
          .api_key = "key", .api_secret = "secret", .passphrase = "phrase"},
      handler, kOrderSendLatencyIterations + 2,
      kOrderSendLatencyIterations + 2);
  if (!ActivateLoginOnly(session)) {
    state.SkipWithError("Bitget order session login setup failed");
    return;
  }
  CountingOrderTransport::ResetStats();

  OrderGatewayWorkerPublisher publisher(0, shm.EventQueue(0));
  OrderGatewayCommandWorker<BenchOrderSession> worker(0, shm.CommandQueue(0),
                                                      session, publisher);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kOrderSendLatencyIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const bool dispatched = worker.PollOnce();
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    if (!dispatched) {
      state.SkipWithError("Bitget gateway worker dispatch failed");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
  }

  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "orders", state.iterations());
  const CountingTransportStats stats = CountingOrderTransport::stats();
  if (stats.invalid_writes != 0 ||
      stats.write_calls != static_cast<std::uint64_t>(state.iterations())) {
    state.SkipWithError("Bitget gateway worker write count mismatch");
  }
  state.counters["bytes_sent"] = static_cast<double>(stats.bytes_sent);
}

BENCHMARK(BenchmarkEncodePlace);
BENCHMARK(BenchmarkEncodeCancel);
BENCHMARK(BenchmarkParsePlaceAck);
BENCHMARK(BenchmarkParsePlaceError);
BENCHMARK(BenchmarkOrderSessionPlaceOrderToCountingTransport)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BenchmarkOrderSessionPlaceAckToCountingHandler)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BenchmarkStrategyContextPlaceLimitOrderToCountingTransport)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BenchmarkGatewayWorkerPlaceOrderToCountingTransport)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila::bitget
