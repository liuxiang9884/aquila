#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "core/websocket/message_view.h"
#include "exchange/binance/market_data/client.h"
#include "exchange/binance/market_data/stream.h"
#include <simdjson.h>

namespace {

constexpr std::string_view kBookTickerJson =
    R"({"e":"bookTicker","u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";

aquila::websocket::MessageView TextView(std::string_view payload) noexcept {
  return {
      .kind = aquila::websocket::PayloadKind::kText,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 8,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

aquila::websocket::MessageView BinaryView(std::string_view payload) noexcept {
  return {
      .kind = aquila::websocket::PayloadKind::kBinary,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 9,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

struct RecordingConsumer {
  int calls{0};
  aquila::BookTicker last{};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++calls;
    last = book_ticker;
  }
};

struct CoarseClockOptions : aquila::websocket::DefaultWebSocketOptions {
  static constexpr aquila::websocket::ClockSource kClockSource =
      aquila::websocket::ClockSource::kMonotonicCoarse;
};

using DefaultClient =
    aquila::binance::FuturesMarketDataClient<RecordingConsumer>;
using DiagnosticClient = aquila::binance::FuturesMarketDataClient<
    RecordingConsumer, aquila::binance::FuturesMarketDataDiagnostics>;
using CoarseClockClient = aquila::binance::FuturesMarketDataClient<
    RecordingConsumer, aquila::binance::NoopFuturesMarketDataDiagnostics,
    CoarseClockOptions>;

static_assert(!DefaultClient::DiagnosticsEnabled);
static_assert(DiagnosticClient::DiagnosticsEnabled);
static_assert(DefaultClient::kClockSource ==
              aquila::websocket::DefaultWebSocketOptions::kClockSource);
static_assert(CoarseClockClient::kClockSource ==
              aquila::websocket::ClockSource::kMonotonicCoarse);
static_assert(!std::is_constructible_v<
              DefaultClient, std::span<const aquila::binance::SymbolBinding>,
              RecordingConsumer&, aquila::websocket::ClockSource>);

}  // namespace

TEST(BinanceFuturesMarketDataClientTest, BuildsBookTickerRawStreamTarget) {
  const std::array<std::string_view, 2> symbols{"BTCUSDT", "ETHUSDT"};

  const std::string target =
      aquila::binance::BuildFuturesBookTickerStreamTarget(symbols);

  EXPECT_EQ(target, "/public/ws/btcusdt@bookTicker/ethusdt@bookTicker");
}

TEST(BinanceFuturesMarketDataClientTest, ExposesStreamCountLimitPredicate) {
  EXPECT_TRUE(aquila::binance::IsValidFuturesBookTickerStreamCount(1));
  EXPECT_TRUE(aquila::binance::IsValidFuturesBookTickerStreamCount(
      aquila::binance::kMaxFuturesBookTickerStreamsPerConnection));
  EXPECT_FALSE(aquila::binance::IsValidFuturesBookTickerStreamCount(0));
  EXPECT_FALSE(aquila::binance::IsValidFuturesBookTickerStreamCount(
      aquila::binance::kMaxFuturesBookTickerStreamsPerConnection + 1));
}

TEST(BinanceFuturesMarketDataClientTest,
     ReturnsEmptyTargetForInvalidStreamCount) {
  const std::array<std::string_view, 0> empty_symbols{};
  std::vector<std::string_view> too_many_symbols(
      aquila::binance::kMaxFuturesBookTickerStreamsPerConnection + 1,
      "BTCUSDT");

  EXPECT_EQ(aquila::binance::BuildFuturesBookTickerStreamTarget(empty_symbols),
            "");
  EXPECT_EQ(aquila::binance::BuildFuturesBookTickerStreamTarget(
                std::span<const std::string_view>(too_many_symbols)),
            "");
}

TEST(BinanceFuturesMarketDataClientTest, EmitsBookTickerFromTextPayload) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  const auto result = client.OnMessage(TextView(kBookTickerJson), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(consumer.last.id, 400900217);
  EXPECT_EQ(consumer.last.exchange_ns, 1568014460893LL * 1'000'000LL);
  EXPECT_EQ(consumer.last.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last.bid_price, 25.3519);
  EXPECT_DOUBLE_EQ(consumer.last.bid_volume, 31.21);
  EXPECT_DOUBLE_EQ(consumer.last.ask_price, 25.3652);
  EXPECT_DOUBLE_EQ(consumer.last.ask_volume, 40.66);
}

TEST(BinanceFuturesMarketDataClientTest, MapsMultipleSymbolsBySymbolText) {
  const std::array<aquila::binance::SymbolBinding, 3> symbols{
      aquila::binance::SymbolBinding{.symbol = "ETHUSDT", .symbol_id = 12},
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11},
      aquila::binance::SymbolBinding{.symbol = "SOLUSDT", .symbol_id = 13}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  const auto result = client.OnTextPayload(kBookTickerJson, 0, 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
}

TEST(BinanceFuturesMarketDataClientTest, ExposesWebSocketMessageCallback) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  const aquila::websocket::MessageCallback callback =
      client.AsMessageCallback();
  const auto result = callback.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_GT(consumer.last.local_ns, 0);
}

TEST(BinanceFuturesMarketDataClientTest, IgnoresBinaryPayload) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  const auto result = client.Handle(BinaryView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 0);
}

TEST(BinanceFuturesMarketDataClientTest, RecordsUnknownSymbolWhenEnabled) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "ETHUSDT", .symbol_id = 12}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  const auto result = client.OnTextPayload(kBookTickerJson, 0, 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 0);
  EXPECT_EQ(client.diagnostics().stats().unknown_symbols, 1U);
}

TEST(BinanceFuturesMarketDataClientTest, RecordsMalformedJsonWhenEnabled) {
  static constexpr std::string_view kMalformedJson =
      R"({"e":"bookTicker","u":400900217,"E":1568014460893,)";
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  const auto result = client.OnTextPayload(kMalformedJson, 0, 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 0);
  EXPECT_EQ(client.diagnostics().stats().malformed_json_messages, 1U);
}

TEST(BinanceFuturesMarketDataClientTest, RecordsPaddingFallbackWhenEnabled) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  const auto result = client.OnTextPayload(kBookTickerJson, 0, 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(client.diagnostics().stats().simdjson_padding_fallback_messages,
            1U);
}

TEST(BinanceFuturesMarketDataClientTest,
     DoesNotRecordPaddingFallbackForPaddedView) {
  std::array<char, kBookTickerJson.size() + simdjson::SIMDJSON_PADDING>
      buffer{};
  std::memcpy(buffer.data(), kBookTickerJson.data(), kBookTickerJson.size());
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  const auto result = client.OnTextPayload(
      std::string_view(buffer.data(), kBookTickerJson.size()),
      static_cast<std::uint32_t>(simdjson::SIMDJSON_PADDING), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(client.diagnostics().stats().simdjson_padding_fallback_messages,
            0U);
}
