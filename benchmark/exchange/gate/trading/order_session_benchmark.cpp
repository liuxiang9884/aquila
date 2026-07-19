#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/trading/order_manager.h"
#include "core/trading/strategy_context.h"
#include "core/websocket/message_view.h"
#include "exchange/gate/trading/order_request_encoder.h"
#include "exchange/gate/trading/order_session.h"
#include "exchange/gate/trading/submit_response_parser.h"
#include <simdjson.h>

namespace aquila::gate {
namespace {

constexpr std::int64_t kTimestamp = 1700000001;
constexpr std::int64_t kLocalOrderId = 12345;
constexpr std::uint64_t kPlaceRequestId = 144115188075855881ULL;
constexpr std::uint64_t kCancelRequestId = 216172782113783818ULL;
constexpr std::uint64_t kExchangeOrderId = 36028827892199865ULL;
constexpr std::size_t kDispatchBatchSize = 512;
constexpr std::size_t kOrderSendLatencyIterations = 4096;
constexpr std::size_t kOrderSendWarmupCount = 512;
constexpr std::size_t kBenchmarkPayloadBufferSize =
    512 + simdjson::SIMDJSON_PADDING;

constexpr std::string_view kLoginSuccess = R"json({
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

constexpr std::string_view kPlaceAck = R"json({
  "request_id": "144115188075855874",
  "ack": true,
  "header": {
    "response_time": "1681195484268",
    "status": "200",
    "channel": "futures.order_place",
    "event": "api"
  },
  "data": {
    "result": {
      "req_id": "144115188075855874"
    }
  }
})json";

constexpr std::string_view kPlaceResult = R"json({
  "request_id": "144115188075855881",
  "ack": false,
  "header": {
    "response_time": "1681195484360",
    "status": "200",
    "channel": "futures.order_place",
    "event": "api",
    "client_id": "::1-0x140001a2600"
  },
  "data": {
    "result": {
      "id": "36028827892199865",
      "status": "open",
      "contract": "BTC_USDT",
      "size": "1",
      "price": "81000",
      "text": "t-12345"
    }
  }
})json";

struct CountingHandler {
  std::uint64_t responses{0};
  OrderResponseKind last_kind{OrderResponseKind::kAck};
  std::uint64_t last_local_order_id{0};
  std::uint64_t last_exchange_order_id{0};

  void OnOrderResponse(const OrderResponse& response) noexcept {
    ++responses;
    last_kind = response.kind;
    last_local_order_id = response.local_order_id;
    last_exchange_order_id = response.exchange_order_id;
  }
};

struct SocketPairWriteStats {
  std::uint64_t open_attempts{0};
  std::uint64_t open_successes{0};
  std::uint64_t invalid_writes{0};
  std::uint64_t write_calls{0};
  std::uint64_t bytes_sent{0};
  std::uint64_t zero_writes{0};
  std::uint64_t send_errors{0};
  int last_errno{0};
  std::uint64_t partial_writes{0};
  std::uint64_t eagain_writes{0};
};

class SocketPairOrderTransport {
 public:
  static constexpr bool kUsesTls = false;

  SocketPairOrderTransport() noexcept {
    (void)OpenPair();
  }

  ~SocketPairOrderTransport() noexcept {
    Close();
  }

  SocketPairOrderTransport(const SocketPairOrderTransport&) = delete;
  SocketPairOrderTransport& operator=(const SocketPairOrderTransport&) = delete;

  bool Init() noexcept {
    return Valid() || OpenPair();
  }

  bool OpenAndConnect(const websocket::ConnectionConfig&) noexcept {
    return Init();
  }

  bool FinishHandshake() noexcept {
    return Valid();
  }

  ssize_t ReadSome(std::span<std::byte>) noexcept {
    wants_read_ = true;
    wants_write_ = false;
    errno = EAGAIN;
    return -1;
  }

  size_t PendingReadableBytes() const noexcept {
    return 0;
  }

  ssize_t WriteSome(std::span<const std::byte> buffer) noexcept {
    wants_read_ = false;
    wants_write_ = false;
    if (!Valid() || buffer.empty()) {
      ++stats_.invalid_writes;
      errno = EBADF;
      return -1;
    }

    ++stats_.write_calls;
    const ssize_t result =
        ::send(client_fd_, buffer.data(), buffer.size(), MSG_NOSIGNAL);
    if (result > 0) {
      stats_.bytes_sent += static_cast<std::uint64_t>(result);
      if (static_cast<std::size_t>(result) < buffer.size()) {
        ++stats_.partial_writes;
      }
      return result;
    }
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      errno = EAGAIN;
      wants_write_ = true;
      ++stats_.eagain_writes;
      stats_.last_errno = EAGAIN;
      return result;
    }
    if (result < 0) {
      ++stats_.send_errors;
      stats_.last_errno = errno;
      return result;
    }
    ++stats_.zero_writes;
    return result;
  }

  bool WantsRead() const noexcept {
    return wants_read_;
  }

  bool WantsWrite() const noexcept {
    return wants_write_;
  }

  int NativeFd() const noexcept {
    return client_fd_;
  }

  void Close() noexcept {
    wants_read_ = false;
    wants_write_ = false;
    if (client_fd_ >= 0) {
      ::close(client_fd_);
      client_fd_ = -1;
    }
    if (peer_fd_ >= 0) {
      if (current_peer_fd_ == peer_fd_) {
        current_peer_fd_ = -1;
      }
      ::close(peer_fd_);
      peer_fd_ = -1;
    }
  }

  static void ResetStats() noexcept {
    stats_ = {};
  }

  [[nodiscard]] static SocketPairWriteStats stats() noexcept {
    return stats_;
  }

  static void DrainPeer() noexcept {
    const int fd = current_peer_fd_;
    if (fd < 0) {
      return;
    }

    std::array<std::byte, 4096> buffer{};
    for (;;) {
      const ssize_t read = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (read > 0) {
        continue;
      }
      if (read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        errno = EAGAIN;
      }
      return;
    }
  }

 private:
  [[nodiscard]] bool OpenPair() noexcept {
    Close();
    ++stats_.open_attempts;

    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      return false;
    }

    constexpr int kSocketBufferBytes = 1 << 20;
    (void)::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &kSocketBufferBytes,
                       sizeof(kSocketBufferBytes));
    (void)::setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &kSocketBufferBytes,
                       sizeof(kSocketBufferBytes));

    if (!websocket::benchmarking::SetNonBlocking(fds[0]) ||
        !websocket::benchmarking::SetNonBlocking(fds[1])) {
      ::close(fds[0]);
      ::close(fds[1]);
      return false;
    }

    client_fd_ = fds[0];
    peer_fd_ = fds[1];
    current_peer_fd_ = peer_fd_;
    ++stats_.open_successes;
    return true;
  }

  [[nodiscard]] bool Valid() const noexcept {
    return client_fd_ >= 0 && peer_fd_ >= 0;
  }

  int client_fd_{-1};
  int peer_fd_{-1};
  bool wants_read_{false};
  bool wants_write_{false};

  inline static int current_peer_fd_{-1};
  inline static SocketPairWriteStats stats_{};
};

struct SocketPairOrderWebSocketPolicy : websocket::DefaultWebSocketOptions {
  using TransportSocket = SocketPairOrderTransport;
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

  size_t PendingReadableBytes() const noexcept {
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

  bool WantsRead() const noexcept {
    return wants_read_;
  }

  bool WantsWrite() const noexcept {
    return wants_write_;
  }

  int NativeFd() const noexcept {
    return -1;
  }

  void Close() noexcept {
    wants_read_ = false;
    wants_write_ = false;
  }

  static void ResetStats() noexcept {
    stats_ = {};
  }

  [[nodiscard]] static SocketPairWriteStats stats() noexcept {
    return stats_;
  }

 private:
  bool wants_read_{false};
  bool wants_write_{false};

  inline static SocketPairWriteStats stats_{};
};

struct CountingOrderWebSocketPolicy : websocket::DefaultWebSocketOptions {
  using TransportSocket = CountingOrderTransport;
};

struct PaddedTextPayload {
  std::array<char, kBenchmarkPayloadBufferSize> data{};
  std::size_t size{0};
};

websocket::MessageView TextView(std::string_view payload,
                                std::uint32_t readable_tail_bytes) noexcept {
  return {
      .kind = websocket::PayloadKind::kText,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 1,
      .fin = true,
      .readable_tail_bytes = readable_tail_bytes,
  };
}

websocket::MessageView TextView(const PaddedTextPayload& payload) noexcept {
  return TextView(std::string_view(payload.data.data(), payload.size),
                  simdjson::SIMDJSON_PADDING);
}

websocket::ConnectionConfig MakeOrderSessionConfig(
    std::size_t prepared_write_slots = 8,
    std::size_t prepared_write_bytes = 4096) {
  websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.port = "80";
  config.target = "/v4/ws/usdt";
  config.prepared_write_slots = prepared_write_slots;
  config.prepared_write_bytes = prepared_write_bytes;
  return config;
}

struct BenchOrder {
  std::uint64_t local_order_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  double quantity{0.0};
  std::string_view quantity_text{};
  std::string_view price_text{};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::uint64_t exchange_order_id{0};
  bool reduce_only{false};
};

BenchOrder MakePlaceOrder() noexcept {
  return BenchOrder{.local_order_id = kLocalOrderId,
                    .symbol = "BTC_USDT",
                    .side = OrderSide::kBuy,
                    .quantity = 1.0,
                    .quantity_text = "1",
                    .price_text = "81000",
                    .time_in_force = TimeInForce::kGoodTillCancel,
                    .exchange_order_id = 0,
                    .reduce_only = false};
}

template <typename Session>
bool ActivateLoginAndPlace(Session& session, std::string_view login_payload,
                           std::size_t place_order_count = 1) noexcept {
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  if (session.Handle(TextView(login_payload, simdjson::SIMDJSON_PADDING)) !=
          websocket::DeliveryResult::kAccepted ||
      !session.login_ready()) {
    return false;
  }
  for (std::size_t i = 0; i < place_order_count; ++i) {
    if (session.PlaceOrder(MakePlaceOrder()).status != OrderSendStatus::kOk) {
      return false;
    }
  }
  return true;
}

template <typename Session>
bool ActivateLoginOnly(Session& session,
                       std::string_view login_payload) noexcept {
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  return session.Handle(TextView(login_payload, simdjson::SIMDJSON_PADDING)) ==
             websocket::DeliveryResult::kAccepted &&
         session.login_ready();
}

[[nodiscard]] constexpr core::OrderCreateRequest
MakeGateLimitRequest() noexcept {
  return core::OrderCreateRequest{.exchange = Exchange::kGate,
                                  .symbol_id = 7,
                                  .symbol = "BTC_USDT",
                                  .side = OrderSide::kBuy,
                                  .time_in_force = TimeInForce::kGoodTillCancel,
                                  .quantity = 1.0,
                                  .quantity_text = "1",
                                  .price_text = "81000",
                                  .reduce_only = false};
}

[[nodiscard]] constexpr core::StrategyOrder MakeStrategyPlaceOrder(
    std::uint64_t local_order_id) noexcept {
  return core::StrategyOrder{
      .local_order_id = local_order_id,
      .exchange_order_id = 0,
      .exchange = Exchange::kGate,
      .symbol_id = 7,
      .symbol = "BTC_USDT",
      .side = OrderSide::kBuy,
      .type = OrderType::kLimit,
      .time_in_force = TimeInForce::kGoodTillCancel,
      .quantity = 1.0,
      .quantity_text = "1",
      .price_text = "81000",
      .reduce_only = false,
  };
}

bool FormatPlaceResultPayload(std::uint64_t sequence,
                              PaddedTextPayload& payload) noexcept {
  const std::uint64_t encoded_request_id =
      RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, sequence);
  const auto result = fmt::format_to_n(
      payload.data.data(), payload.data.size() - simdjson::SIMDJSON_PADDING,
      R"json({{"request_id":"{}","ack":false,"header":{{"response_time":"1681195484360","status":"200","channel":"futures.order_place","event":"api"}},"data":{{"result":{{"id":"{}","status":"open","contract":"BTC_USDT","size":"1","price":"81000","text":"t-{}"}}}}}})json",
      encoded_request_id, kExchangeOrderId, kLocalOrderId);
  if (result.size > payload.data.size() - simdjson::SIMDJSON_PADDING) {
    return false;
  }
  payload.size = result.size;
  return true;
}

void BM_EncodePlaceOrder(benchmark::State& state) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  const PlaceOrderEncodeFields fields{
      .timestamp = kTimestamp,
      .encoded_request_id = kPlaceRequestId,
      .local_order_id = kLocalOrderId,
      .contract = "BTC_USDT",
      .signed_size_text = "1",
      .price_text = "81000",
      .time_in_force = TimeInForce::kGoodTillCancel,
      .reduce_only = false,
  };
  const EncodedTextRequest sample = EncodePlaceOrderRequest(fields, buffer);
  if (sample.status != OrderEncodeStatus::kOk) {
    state.SkipWithError("place order sample encode failed");
    return;
  }
  const std::size_t encoded_size = sample.text.size();

  for (auto _ : state) {
    const EncodedTextRequest encoded = EncodePlaceOrderRequest(fields, buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      state.SkipWithError("place order encode failed");
      return;
    }
    benchmark::DoNotOptimize(encoded.text.data());
    benchmark::DoNotOptimize(encoded.text.size());
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(encoded_size));
}

void BM_EncodeCancelOrder(benchmark::State& state) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  const CancelOrderEncodeFields fields{
      .timestamp = kTimestamp,
      .encoded_request_id = kCancelRequestId,
      .local_order_id = kLocalOrderId,
      .exchange_order_id = kExchangeOrderId,
  };
  const EncodedTextRequest sample = EncodeCancelOrderRequest(fields, buffer);
  if (sample.status != OrderEncodeStatus::kOk) {
    state.SkipWithError("cancel order sample encode failed");
    return;
  }
  const std::size_t encoded_size = sample.text.size();

  for (auto _ : state) {
    const EncodedTextRequest encoded = EncodeCancelOrderRequest(fields, buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      state.SkipWithError("cancel order encode failed");
      return;
    }
    benchmark::DoNotOptimize(encoded.text.data());
    benchmark::DoNotOptimize(encoded.text.size());
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(encoded_size));
}

void BM_ParsePlaceResult(benchmark::State& state) {
  std::array<char, kPlaceResult.size() + simdjson::SIMDJSON_PADDING> payload{};
  std::copy(kPlaceResult.begin(), kPlaceResult.end(), payload.begin());
  simdjson::ondemand::parser parser;

  for (auto _ : state) {
    const GateSubmitResponse parsed = ParseGateSubmitResponse(
        std::string_view(payload.data(), kPlaceResult.size()),
        simdjson::SIMDJSON_PADDING, parser);
    if (parsed.parse_status != GateSubmitParseStatus::kOk ||
        parsed.kind != GateSubmitResponseKind::kResult ||
        !parsed.request_id.ok || !parsed.has_local_order_id) {
      state.SkipWithError("place result parse failed");
      return;
    }
    std::uint64_t exchange_order_id = parsed.exchange_order_id;
    std::uint64_t local_order_id = parsed.local_order_id;
    benchmark::DoNotOptimize(exchange_order_id);
    benchmark::DoNotOptimize(local_order_id);
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kPlaceResult.size()));
}

void BM_ParsePlaceResultForOrderSession(benchmark::State& state) {
  std::array<char, kPlaceResult.size() + simdjson::SIMDJSON_PADDING> payload{};
  std::copy(kPlaceResult.begin(), kPlaceResult.end(), payload.begin());
  simdjson::ondemand::parser parser;

  for (auto _ : state) {
    const GateSubmitResponse parsed = ParseGateSubmitResponseForOrderSession(
        std::string_view(payload.data(), kPlaceResult.size()),
        simdjson::SIMDJSON_PADDING, parser);
    if (parsed.parse_status != GateSubmitParseStatus::kOk ||
        parsed.kind != GateSubmitResponseKind::kResult ||
        !parsed.request_id.ok || !parsed.has_local_order_id) {
      state.SkipWithError("place result parse failed");
      return;
    }
    std::uint64_t exchange_order_id = parsed.exchange_order_id;
    std::uint64_t local_order_id = parsed.local_order_id;
    benchmark::DoNotOptimize(exchange_order_id);
    benchmark::DoNotOptimize(local_order_id);
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kPlaceResult.size()));
}

void BM_OrderSessionHandlePlaceAck(benchmark::State& state) {
  std::array<char, kLoginSuccess.size() + simdjson::SIMDJSON_PADDING>
      login_payload{};
  std::copy(kLoginSuccess.begin(), kLoginSuccess.end(), login_payload.begin());
  std::array<char, kPlaceAck.size() + simdjson::SIMDJSON_PADDING> ack_payload{};
  std::copy(kPlaceAck.begin(), kPlaceAck.end(), ack_payload.begin());

  CountingHandler handler;
  OrderSession<CountingHandler, OrderSessionDefaultPlainWebSocketPolicy>
      session(MakeOrderSessionConfig(),
              LoginCredentials{.api_key = "key", .api_secret = "secret"},
              handler);
  if (!ActivateLoginAndPlace(session, std::string_view(login_payload.data(),
                                                       kLoginSuccess.size()))) {
    state.SkipWithError("order session setup failed");
    return;
  }

  const auto view =
      TextView(std::string_view(ack_payload.data(), kPlaceAck.size()),
               simdjson::SIMDJSON_PADDING);
  for (auto _ : state) {
    const websocket::DeliveryResult result = session.Handle(view);
    if (result != websocket::DeliveryResult::kAccepted) {
      state.SkipWithError("ack dispatch failed");
      return;
    }
    benchmark::DoNotOptimize(handler.responses);
    benchmark::DoNotOptimize(handler.last_local_order_id);
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(kPlaceAck.size()));
}

void BM_OrderSessionHandlePlaceResult(benchmark::State& state) {
  std::array<char, kLoginSuccess.size() + simdjson::SIMDJSON_PADDING>
      login_payload{};
  std::copy(kLoginSuccess.begin(), kLoginSuccess.end(), login_payload.begin());
  std::array<PaddedTextPayload, kDispatchBatchSize> result_payloads{};
  for (std::size_t i = 0; i < result_payloads.size(); ++i) {
    if (!FormatPlaceResultPayload(i + 2, result_payloads[i])) {
      state.SkipWithError("place result payload format failed");
      return;
    }
  }

  CountingHandler handler;
  using BenchOrderSession =
      OrderSession<CountingHandler, OrderSessionDefaultPlainWebSocketPolicy>;
  std::optional<BenchOrderSession> session;
  std::size_t payload_index = kDispatchBatchSize;
  for (auto _ : state) {
    if (payload_index == kDispatchBatchSize) {
      state.PauseTiming();
      session.emplace(
          MakeOrderSessionConfig(kDispatchBatchSize + 1, 1U << 20),
          LoginCredentials{.api_key = "key", .api_secret = "secret"}, handler);
      if (!ActivateLoginAndPlace(
              *session,
              std::string_view(login_payload.data(), kLoginSuccess.size()),
              kDispatchBatchSize)) {
        state.SkipWithError("order session setup failed");
        return;
      }
      payload_index = 0;
      state.ResumeTiming();
    }

    const websocket::DeliveryResult result =
        session->Handle(TextView(result_payloads[payload_index]));
    if (result != websocket::DeliveryResult::kAccepted) {
      state.SkipWithError("result dispatch failed");
      return;
    }
    ++payload_index;
    benchmark::DoNotOptimize(handler.responses);
    benchmark::DoNotOptimize(handler.last_exchange_order_id);
    if (payload_index == kDispatchBatchSize) {
      state.PauseTiming();
      if (session->inflight_count() != 0) {
        state.SkipWithError("result dispatch did not drain inflight orders");
        return;
      }
      session.reset();
      state.ResumeTiming();
    }
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(result_payloads[0].size));
}

void SetSocketPairWriteCounters(benchmark::State& state,
                                SocketPairWriteStats stats) {
  state.counters["write_calls"] = static_cast<double>(stats.write_calls);
  state.counters["bytes_sent"] = static_cast<double>(stats.bytes_sent);
  state.counters["zero_writes"] = static_cast<double>(stats.zero_writes);
  state.counters["send_errors"] = static_cast<double>(stats.send_errors);
  state.counters["last_errno"] = static_cast<double>(stats.last_errno);
  state.counters["open_attempts"] = static_cast<double>(stats.open_attempts);
  state.counters["open_successes"] = static_cast<double>(stats.open_successes);
  state.counters["invalid_writes"] = static_cast<double>(stats.invalid_writes);
  state.counters["partial_writes"] = static_cast<double>(stats.partial_writes);
  state.counters["eagain_writes"] = static_cast<double>(stats.eagain_writes);
}

void SetOutlierCounters(benchmark::State& state,
                        std::span<const std::uint64_t> samples_ns) {
  std::uint64_t over_1us = 0;
  std::uint64_t over_1us_first_256 = 0;
  std::uint64_t over_1us_first_512 = 0;
  std::uint64_t over_1us_after_512 = 0;
  std::uint64_t over_2us = 0;
  std::uint64_t over_5us = 0;
  std::uint64_t max_index = 0;
  std::uint64_t max_ns = 0;
  std::uint64_t last_over_1us_index = 0;

  for (std::size_t i = 0; i < samples_ns.size(); ++i) {
    const std::uint64_t sample_ns = samples_ns[i];
    if (sample_ns > 1'000) {
      ++over_1us;
      over_1us_first_256 += i < 256 ? 1 : 0;
      over_1us_first_512 += i < 512 ? 1 : 0;
      over_1us_after_512 += i >= 512 ? 1 : 0;
      last_over_1us_index = static_cast<std::uint64_t>(i);
    }
    over_2us += sample_ns > 2'000 ? 1 : 0;
    over_5us += sample_ns > 5'000 ? 1 : 0;
    if (sample_ns > max_ns) {
      max_ns = sample_ns;
      max_index = static_cast<std::uint64_t>(i);
    }
  }

  state.counters["over_1us"] = static_cast<double>(over_1us);
  state.counters["over_1us_first_256"] =
      static_cast<double>(over_1us_first_256);
  state.counters["over_1us_first_512"] =
      static_cast<double>(over_1us_first_512);
  state.counters["over_1us_after_512"] =
      static_cast<double>(over_1us_after_512);
  state.counters["over_2us"] = static_cast<double>(over_2us);
  state.counters["over_5us"] = static_cast<double>(over_5us);
  state.counters["max_index"] = static_cast<double>(max_index);
  state.counters["last_over_1us"] = static_cast<double>(last_over_1us_index);
}

[[nodiscard]] bool HasSocketWriteFailure(SocketPairWriteStats stats) noexcept {
  return stats.invalid_writes != 0 || stats.send_errors != 0 ||
         stats.eagain_writes != 0 || stats.zero_writes != 0 ||
         stats.partial_writes != 0;
}

template <typename Transport>
void DrainTransportPeer() noexcept {
  if constexpr (requires { Transport::DrainPeer(); }) {
    Transport::DrainPeer();
  }
}

template <typename Transport>
[[nodiscard]] bool ValidateLoginSocketWrite(benchmark::State& state) {
  const SocketPairWriteStats stats = Transport::stats();
  if (stats.write_calls != 1 || stats.bytes_sent == 0 ||
      HasSocketWriteFailure(stats)) {
    SetSocketPairWriteCounters(state, stats);
    state.SkipWithError("login socket write failed");
    return false;
  }
  return true;
}

template <typename Transport>
[[nodiscard]] bool ValidateOrderSocketWrites(benchmark::State& state,
                                             std::uint64_t expected_writes) {
  const SocketPairWriteStats stats = Transport::stats();
  if (stats.write_calls != expected_writes || stats.bytes_sent == 0 ||
      HasSocketWriteFailure(stats)) {
    SetSocketPairWriteCounters(state, stats);
    state.SkipWithError("order socket write count mismatch");
    return false;
  }
  SetSocketPairWriteCounters(state, stats);
  return true;
}

template <typename WebSocketPolicy>
void RunOrderSessionPlaceOrderWriteBenchmark(benchmark::State& state,
                                             std::size_t warmup_count = 0) {
  using Transport = typename WebSocketPolicy::TransportSocket;
  Transport::ResetStats();
  CountingHandler handler;
  using BenchOrderSession = OrderSession<CountingHandler, WebSocketPolicy>;
  BenchOrderSession session(
      MakeOrderSessionConfig(kOrderSendLatencyIterations + warmup_count + 2,
                             1U << 20),
      LoginCredentials{.api_key = "key", .api_secret = "secret"}, handler,
      kOrderSendLatencyIterations + warmup_count + 2);
  if (!ActivateLoginOnly(session, kLoginSuccess)) {
    state.SkipWithError("order session login setup failed");
    return;
  }
  if (!ValidateLoginSocketWrite<Transport>(state)) {
    return;
  }
  DrainTransportPeer<Transport>();
  Transport::ResetStats();

  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kOrderSendLatencyIterations);
  std::uint64_t local_order_id = kLocalOrderId;

  for (std::size_t i = 0; i < warmup_count; ++i) {
    BenchOrder order = MakePlaceOrder();
    order.local_order_id = local_order_id++;
    if (session.PlaceOrder(order).status != OrderSendStatus::kOk) {
      state.SkipWithError("order session warmup place order failed");
      return;
    }
    DrainTransportPeer<Transport>();
  }
  Transport::ResetStats();

  for (auto _ : state) {
    BenchOrder order = MakePlaceOrder();
    order.local_order_id = local_order_id++;

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const OrderSendResult result = session.PlaceOrder(order);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    if (result.status != OrderSendStatus::kOk) {
      state.SkipWithError("order session place order failed");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);

    state.PauseTiming();
    DrainTransportPeer<Transport>();
    state.ResumeTiming();
    std::uint64_t encoded_request_id = result.encoded_request_id;
    benchmark::DoNotOptimize(encoded_request_id);
  }

  SetOutlierCounters(state, samples_ns);
  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "orders", state.iterations());
  (void)ValidateOrderSocketWrites<Transport>(
      state, static_cast<std::uint64_t>(state.iterations()));
}

template <typename WebSocketPolicy>
void RunStrategyContextPlaceLimitOrderWriteBenchmark(
    benchmark::State& state, std::size_t warmup_count = 0) {
  using Transport = typename WebSocketPolicy::TransportSocket;
  Transport::ResetStats();
  CountingHandler handler;
  using BenchOrderSession = OrderSession<CountingHandler, WebSocketPolicy>;
  BenchOrderSession session(
      MakeOrderSessionConfig(kOrderSendLatencyIterations + warmup_count + 2,
                             1U << 20),
      LoginCredentials{.api_key = "key", .api_secret = "secret"}, handler,
      kOrderSendLatencyIterations + warmup_count + 2);
  if (!ActivateLoginOnly(session, kLoginSuccess)) {
    state.SkipWithError("order session login setup failed");
    return;
  }
  if (!ValidateLoginSocketWrite<Transport>(state)) {
    return;
  }
  core::OrderManager<BenchOrderSession> order_manager(
      session, kOrderSendLatencyIterations + warmup_count + 2);
  core::StrategyContext<BenchOrderSession> context(order_manager);
  DrainTransportPeer<Transport>();
  Transport::ResetStats();

  for (std::size_t i = 0; i < warmup_count; ++i) {
    if (context.PlaceLimitOrder(MakeGateLimitRequest()).status !=
        core::OrderPlaceStatus::kOk) {
      state.SkipWithError("strategy warmup place limit order failed");
      return;
    }
    DrainTransportPeer<Transport>();
  }
  Transport::ResetStats();

  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kOrderSendLatencyIterations);

  for (auto _ : state) {
    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const core::OrderPlaceResult result =
        context.PlaceLimitOrder(MakeGateLimitRequest());
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    if (result.status != core::OrderPlaceStatus::kOk) {
      state.SkipWithError("strategy place limit order failed");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);

    state.PauseTiming();
    DrainTransportPeer<Transport>();
    state.ResumeTiming();
    std::uint64_t local_order_id = result.local_order_id;
    benchmark::DoNotOptimize(local_order_id);
  }

  SetOutlierCounters(state, samples_ns);
  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "orders", state.iterations());
  (void)ValidateOrderSocketWrites<Transport>(
      state, static_cast<std::uint64_t>(state.iterations()));
}

void BM_OrderSessionPlaceOrderToSocketPair(benchmark::State& state) {
  RunOrderSessionPlaceOrderWriteBenchmark<SocketPairOrderWebSocketPolicy>(
      state);
}

void BM_StrategyContextPlaceLimitOrderToSocketPair(benchmark::State& state) {
  RunStrategyContextPlaceLimitOrderWriteBenchmark<
      SocketPairOrderWebSocketPolicy>(state);
}

void BM_StrategyContextPlaceLimitOrderToSocketPairWarm(
    benchmark::State& state) {
  RunStrategyContextPlaceLimitOrderWriteBenchmark<
      SocketPairOrderWebSocketPolicy>(state, kOrderSendWarmupCount);
}

void BM_OrderSessionPlaceOrderToCountingTransport(benchmark::State& state) {
  RunOrderSessionPlaceOrderWriteBenchmark<CountingOrderWebSocketPolicy>(state);
}

void BM_OrderSessionPlaceStrategyOrderToCountingTransport(
    benchmark::State& state) {
  using Transport = CountingOrderTransport;
  Transport::ResetStats();
  CountingHandler handler;
  using BenchOrderSession =
      OrderSession<CountingHandler, CountingOrderWebSocketPolicy>;
  BenchOrderSession session(
      MakeOrderSessionConfig(kOrderSendLatencyIterations + 2, 1U << 20),
      LoginCredentials{.api_key = "key", .api_secret = "secret"}, handler,
      kOrderSendLatencyIterations + 2);
  if (!ActivateLoginOnly(session, kLoginSuccess)) {
    state.SkipWithError("order session login setup failed");
    return;
  }
  if (!ValidateLoginSocketWrite<Transport>(state)) {
    return;
  }
  Transport::ResetStats();

  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kOrderSendLatencyIterations);
  std::uint64_t strategy_order_id = 1;

  for (auto _ : state) {
    const core::StrategyOrder order =
        MakeStrategyPlaceOrder(strategy_order_id++);

    const std::uint64_t start_ns = websocket::benchmarking::NowNs();
    const OrderSendResult result = session.PlaceOrder(order);
    const std::uint64_t elapsed_ns =
        websocket::benchmarking::NowNs() - start_ns;
    if (result.status != OrderSendStatus::kOk) {
      state.SkipWithError("order session place order failed");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);

    std::uint64_t encoded_request_id = result.encoded_request_id;
    benchmark::DoNotOptimize(encoded_request_id);
  }

  SetOutlierCounters(state, samples_ns);
  websocket::benchmarking::SetLatencyCounters(state, std::move(samples_ns),
                                              "orders", state.iterations());
  (void)ValidateOrderSocketWrites<Transport>(
      state, static_cast<std::uint64_t>(state.iterations()));
}

void BM_StrategyContextPlaceLimitOrderToCountingTransport(
    benchmark::State& state) {
  RunStrategyContextPlaceLimitOrderWriteBenchmark<CountingOrderWebSocketPolicy>(
      state);
}

void BM_StrategyContextPlaceLimitOrderToCountingTransportWarm(
    benchmark::State& state) {
  RunStrategyContextPlaceLimitOrderWriteBenchmark<CountingOrderWebSocketPolicy>(
      state, kOrderSendWarmupCount);
}

BENCHMARK(BM_EncodePlaceOrder);
BENCHMARK(BM_EncodeCancelOrder);
BENCHMARK(BM_ParsePlaceResult);
BENCHMARK(BM_ParsePlaceResultForOrderSession);
BENCHMARK(BM_OrderSessionHandlePlaceAck);
BENCHMARK(BM_OrderSessionHandlePlaceResult)
    ->Iterations(kOrderSendLatencyIterations);
BENCHMARK(BM_OrderSessionPlaceOrderToSocketPair)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_StrategyContextPlaceLimitOrderToSocketPair)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_StrategyContextPlaceLimitOrderToSocketPairWarm)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderSessionPlaceOrderToCountingTransport)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderSessionPlaceStrategyOrderToCountingTransport)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_StrategyContextPlaceLimitOrderToCountingTransport)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_StrategyContextPlaceLimitOrderToCountingTransportWarm)
    ->Iterations(kOrderSendLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila::gate
