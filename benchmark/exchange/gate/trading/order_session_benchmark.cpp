#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

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
  std::int64_t last_local_order_id{0};
  std::uint64_t last_exchange_order_id{0};

  void OnOrderResponse(const OrderResponse& response) noexcept {
    ++responses;
    last_kind = response.kind;
    last_local_order_id = response.local_order_id;
    last_exchange_order_id = response.exchange_order_id;
  }
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
  config.service = "80";
  config.target = "/v4/ws/usdt";
  config.prepared_write_slots = prepared_write_slots;
  config.prepared_write_bytes = prepared_write_bytes;
  return config;
}

PlaceOrderRequest MakePlaceOrderRequest() noexcept {
  return PlaceOrderRequest{.wire = OrderWireFields{
                               .local_order_id = kLocalOrderId,
                               .contract = "BTC_USDT",
                               .signed_size = 1,
                               .price_text = "81000",
                               .tif = "gtc",
                               .text = "t-12345",
                               .reduce_only = false,
                           }};
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
    if (session.PlaceOrder(MakePlaceOrderRequest()).status !=
        OrderSendStatus::kOk) {
      return false;
    }
  }
  return true;
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
      .wire =
          {
              .local_order_id = kLocalOrderId,
              .contract = "BTC_USDT",
              .signed_size = 1,
              .price_text = "81000",
              .tif = "gtc",
              .text = "t-12345",
              .reduce_only = false,
          },
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
    std::int64_t local_order_id = parsed.local_order_id;
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
    std::int64_t local_order_id = parsed.local_order_id;
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

BENCHMARK(BM_EncodePlaceOrder);
BENCHMARK(BM_EncodeCancelOrder);
BENCHMARK(BM_ParsePlaceResult);
BENCHMARK(BM_ParsePlaceResultForOrderSession);
BENCHMARK(BM_OrderSessionHandlePlaceAck);
BENCHMARK(BM_OrderSessionHandlePlaceResult);

}  // namespace
}  // namespace aquila::gate
