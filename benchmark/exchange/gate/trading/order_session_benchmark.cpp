#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

#include <benchmark/benchmark.h>

#include "exchange/gate/trading/order_request_encoder.h"
#include "exchange/gate/trading/submit_response_parser.h"
#include <simdjson.h>

namespace aquila::gate {
namespace {

constexpr std::int64_t kTimestamp = 1700000001;
constexpr std::int64_t kLocalOrderId = 12345;
constexpr std::uint64_t kPlaceRequestId = 144115188075855881ULL;
constexpr std::uint64_t kCancelRequestId = 216172782113783818ULL;
constexpr std::uint64_t kExchangeOrderId = 36028827892199865ULL;

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
    const GateSubmitResponse parsed =
        ParseGateSubmitResponse(payload, kPlaceResult.size(), parser);
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

BENCHMARK(BM_EncodePlaceOrder);
BENCHMARK(BM_EncodeCancelOrder);
BENCHMARK(BM_ParsePlaceResult);

}  // namespace
}  // namespace aquila::gate
