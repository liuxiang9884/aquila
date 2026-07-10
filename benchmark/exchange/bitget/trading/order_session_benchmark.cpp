#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

#include <benchmark/benchmark.h>

#include "core/common/types.h"
#include "exchange/bitget/trading/operation_response_parser.h"
#include "exchange/bitget/trading/order_request_encoder.h"
#include <simdjson.h>

namespace aquila::bitget {
namespace {

constexpr std::string_view kPlaceAck =
    R"({"event":"trade","id":"72057594037927945","category":"usdt-futures","topic":"place-order","args":[{"symbol":"BTCUSDT","orderId":"123456789","clientOid":"a-42","cTime":"1750034397008"}],"code":"0","msg":"success","connId":"connection-1","ts":"1750034397076"})";

constexpr std::string_view kPlaceError =
    R"({"event":"error","id":"72057594037927945","topic":"place-order","code":"40010","msg":"Request timed out","ts":"1750034397076"})";

void BenchmarkEncodePlace(benchmark::State& state) {
  const PlaceOrderEncodeFields fields{
      .encoded_request_id = 72057594037927945ULL,
      .local_order_id = 42,
      .order_type = OrderType::kLimit,
      .symbol = "BTCUSDT",
      .quantity_text = "0.001",
      .price_text = "100000.0",
      .side = OrderSide::kBuy,
      .time_in_force = TimeInForce::kImmediateOrCancel,
  };
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  for (auto _ : state) {
    const EncodedTextRequest encoded = EncodePlaceOrderRequest(fields, buffer);
    benchmark::DoNotOptimize(encoded.text.data());
    benchmark::DoNotOptimize(encoded.text.size());
  }
}

void BenchmarkEncodeCancel(benchmark::State& state) {
  const CancelOrderEncodeFields fields{
      .encoded_request_id = 144115188075855881ULL,
      .local_order_id = 42,
      .exchange_order_id = 123456789,
  };
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  for (auto _ : state) {
    const EncodedTextRequest encoded = EncodeCancelOrderRequest(fields, buffer);
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

BENCHMARK(BenchmarkEncodePlace);
BENCHMARK(BenchmarkEncodeCancel);
BENCHMARK(BenchmarkParsePlaceAck);
BENCHMARK(BenchmarkParsePlaceError);

}  // namespace
}  // namespace aquila::bitget
