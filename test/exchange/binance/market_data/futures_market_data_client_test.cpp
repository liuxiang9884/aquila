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
constexpr std::string_view kTradeJson =
    R"({"e":"trade","E":1783228448495,"T":1783228448495,"s":"BTCUSDT","t":7868321828,"p":"62738.70","q":"0.002","X":"MARKET","m":false,"st":1})";
constexpr std::string_view kSellTakerTradeJson =
    R"({"e":"trade","E":1783228448434,"T":1783228448434,"s":"ETHUSDT","t":8412341979,"p":"1765.20","q":"0.109","X":"MARKET","m":true,"st":1})";
constexpr std::string_view kMissingEventJson =
    R"({"u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";
constexpr std::string_view kUnsupportedEventJson =
    R"({"e":"aggTrade","E":1783228313782,"a":3371116472,"s":"BTCUSDT","p":"62733.90","q":"0.001","nq":"0.001","f":7868320969,"l":7868320969,"T":1783228313630,"m":false,"st":1})";

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
  int book_ticker_calls{0};
  int trade_calls{0};
  aquila::BookTicker last_book_ticker{};
  aquila::Trade last_trade{};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++book_ticker_calls;
    last_book_ticker = book_ticker;
  }

  void OnTrade(const aquila::Trade& trade) noexcept {
    ++trade_calls;
    last_trade = trade;
  }
};

struct EmplaceOnlyConsumer {
  int book_ticker_emplace_calls{0};
  int trade_emplace_calls{0};
  aquila::BookTicker last_book_ticker{};
  aquila::Trade last_trade{};

  void OnBookTicker(const aquila::BookTicker&) noexcept = delete;
  void OnTrade(const aquila::Trade&) noexcept = delete;

  template <typename Writer>
  void EmplaceBookTickerWith(Writer&& writer) noexcept {
    ++book_ticker_emplace_calls;
    writer(last_book_ticker);
  }

  template <typename Writer>
  void EmplaceTradeWith(Writer&& writer) noexcept {
    ++trade_emplace_calls;
    writer(last_trade);
  }
};

struct BookTickerOnlyConsumer {
  int book_ticker_calls{0};
  aquila::BookTicker last_book_ticker{};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++book_ticker_calls;
    last_book_ticker = book_ticker;
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

TEST(BinanceFuturesMarketDataClientTest, BuildsMixedRawStreamTarget) {
  const std::array<std::string_view, 2> symbols{"BTCUSDT", "ETHUSDT"};
  const aquila::binance::DataSessionFeeds feeds{.book_ticker = true,
                                                .trade = true};

  const std::string target =
      aquila::binance::BuildFuturesMarketDataStreamTarget(symbols, feeds);

  EXPECT_EQ(target,
            "/public/ws/btcusdt@bookTicker/btcusdt@trade/"
            "ethusdt@bookTicker/ethusdt@trade");
}

TEST(BinanceFuturesMarketDataClientTest, BuildsTradeOnlyRawStreamTarget) {
  const std::array<std::string_view, 2> symbols{"BTCUSDT", "ETHUSDT"};
  const aquila::binance::DataSessionFeeds feeds{.book_ticker = false,
                                                .trade = true};

  const std::string target =
      aquila::binance::BuildFuturesMarketDataStreamTarget(symbols, feeds);

  EXPECT_EQ(target, "/public/ws/btcusdt@trade/ethusdt@trade");
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
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(consumer.last_book_ticker.symbol_id, 11);
  EXPECT_EQ(consumer.last_book_ticker.exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(consumer.last_book_ticker.id, 400900217);
  EXPECT_EQ(consumer.last_book_ticker.exchange_ns,
            1568014460893LL * 1'000'000LL);
  EXPECT_EQ(consumer.last_book_ticker.event_ns,
            1568014460891LL * 1'000'000LL);
  EXPECT_EQ(consumer.last_book_ticker.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last_book_ticker.bid_price, 25.3519);
  EXPECT_DOUBLE_EQ(consumer.last_book_ticker.bid_volume, 31.21);
  EXPECT_DOUBLE_EQ(consumer.last_book_ticker.ask_price, 25.3652);
  EXPECT_DOUBLE_EQ(consumer.last_book_ticker.ask_volume, 40.66);
}

TEST(BinanceFuturesMarketDataClientTest,
     EmplacesBookTickerWhenConsumerSupportsSlotWriter) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  EmplaceOnlyConsumer consumer;
  aquila::binance::FuturesMarketDataClient client(symbols, consumer);

  const auto result = client.OnMessage(TextView(kBookTickerJson), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.book_ticker_emplace_calls, 1);
  EXPECT_EQ(consumer.last_book_ticker.symbol_id, 11);
  EXPECT_EQ(consumer.last_book_ticker.exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(consumer.last_book_ticker.id, 400900217);
  EXPECT_EQ(consumer.last_book_ticker.exchange_ns,
            1568014460893LL * 1'000'000LL);
  EXPECT_EQ(consumer.last_book_ticker.event_ns,
            1568014460891LL * 1'000'000LL);
  EXPECT_EQ(consumer.last_book_ticker.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last_book_ticker.bid_price, 25.3519);
  EXPECT_DOUBLE_EQ(consumer.last_book_ticker.bid_volume, 31.21);
  EXPECT_DOUBLE_EQ(consumer.last_book_ticker.ask_price, 25.3652);
  EXPECT_DOUBLE_EQ(consumer.last_book_ticker.ask_volume, 40.66);
}

TEST(BinanceFuturesMarketDataClientTest, EmitsTradeFromTextPayload) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  const auto result = client.OnMessage(TextView(kTradeJson), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.trade_calls, 1);
  EXPECT_EQ(consumer.last_trade.symbol_id, 11);
  EXPECT_EQ(consumer.last_trade.exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(consumer.last_trade.id, 7868321828LL);
  EXPECT_EQ(consumer.last_trade.side, aquila::OrderSide::kBuy);
  EXPECT_EQ(consumer.last_trade.exchange_ns, 1783228448495LL * 1'000'000LL);
  EXPECT_EQ(consumer.last_trade.event_ns, 1783228448495LL * 1'000'000LL);
  EXPECT_EQ(consumer.last_trade.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last_trade.price, 62738.70);
  EXPECT_DOUBLE_EQ(consumer.last_trade.volume, 0.002);
  EXPECT_EQ(consumer.last_trade.batch_index, 0U);
  EXPECT_EQ(consumer.last_trade.batch_count, 1U);
}

TEST(BinanceFuturesMarketDataClientTest, MapsBuyerMakerTradeToSellTakerSide) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "ETHUSDT", .symbol_id = 12}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  const auto result = client.OnMessage(TextView(kSellTakerTradeJson), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.trade_calls, 1);
  EXPECT_EQ(consumer.last_trade.symbol_id, 12);
  EXPECT_EQ(consumer.last_trade.side, aquila::OrderSide::kSell);
}

TEST(BinanceFuturesMarketDataClientTest,
     EmplacesTradeWhenConsumerSupportsSlotWriter) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  EmplaceOnlyConsumer consumer;
  aquila::binance::FuturesMarketDataClient client(symbols, consumer);

  const auto result = client.OnMessage(TextView(kTradeJson), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.trade_emplace_calls, 1);
  EXPECT_EQ(consumer.last_trade.symbol_id, 11);
  EXPECT_EQ(consumer.last_trade.exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(consumer.last_trade.id, 7868321828LL);
  EXPECT_EQ(consumer.last_trade.side, aquila::OrderSide::kBuy);
}

TEST(BinanceFuturesMarketDataClientTest, DropsTradeWhenFeedDisabled) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  EmplaceOnlyConsumer consumer;
  aquila::binance::FuturesMarketDataClient<
      EmplaceOnlyConsumer, aquila::binance::FuturesMarketDataDiagnostics>
      client(symbols,
             aquila::binance::DataSessionFeeds{.book_ticker = true,
                                               .trade = false},
             consumer);
  bool decoded_book_ticker = true;
  bool decoded_trade = true;

  const auto result = client.OnTextPayload(
      kTradeJson, 0, 999'000, &decoded_book_ticker, &decoded_trade);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.trade_emplace_calls, 0);
  EXPECT_FALSE(decoded_book_ticker);
  EXPECT_FALSE(decoded_trade);
  EXPECT_EQ(client.diagnostics().stats().unsupported_event_messages, 1U);
  EXPECT_EQ(client.diagnostics().stats().trade_messages, 0U);
}

TEST(BinanceFuturesMarketDataClientTest,
     DropsTradeWhenConsumerDoesNotSupportTradeSink) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  BookTickerOnlyConsumer consumer;
  aquila::binance::FuturesMarketDataClient<
      BookTickerOnlyConsumer, aquila::binance::FuturesMarketDataDiagnostics>
      client(symbols, consumer);

  const auto result = client.OnMessage(TextView(kTradeJson), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
  EXPECT_EQ(client.diagnostics().stats().trade_messages, 1U);
}

TEST(BinanceFuturesMarketDataClientTest, RecordsUnsupportedEventWhenEnabled) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  const auto result = client.OnTextPayload(kUnsupportedEventJson, 0, 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
  EXPECT_EQ(consumer.trade_calls, 0);
  EXPECT_EQ(client.diagnostics().stats().unsupported_event_messages, 1U);
}

TEST(BinanceFuturesMarketDataClientTest,
     RecordsMalformedJsonWhenEventTypeMissing) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  const auto result = client.OnTextPayload(kMissingEventJson, 0, 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
  EXPECT_EQ(consumer.trade_calls, 0);
  EXPECT_EQ(client.diagnostics().stats().malformed_json_messages, 1U);
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
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(consumer.last_book_ticker.symbol_id, 11);
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
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_GT(consumer.last_book_ticker.local_ns, 0);
}

TEST(BinanceFuturesMarketDataClientTest, IgnoresBinaryPayload) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  const auto result = client.Handle(BinaryView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
  EXPECT_EQ(consumer.trade_calls, 0);
}

TEST(BinanceFuturesMarketDataClientTest, AssertsUnknownSymbolInDebug) {
#ifndef NDEBUG
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "ETHUSDT", .symbol_id = 12}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  EXPECT_DEATH((void)client.OnTextPayload(kBookTickerJson, 0, 999'000), "");
#else
  GTEST_SKIP() << "Unknown symbol is a debug assert contract.";
#endif
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
  EXPECT_EQ(consumer.book_ticker_calls, 0);
  EXPECT_EQ(consumer.trade_calls, 0);
  EXPECT_EQ(client.diagnostics().stats().malformed_json_messages, 1U);
}

TEST(BinanceFuturesMarketDataClientTest, RecordsPaddingFallbackWhenEnabled) {
  const std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  const auto result = client.OnTextPayload(kBookTickerJson, 0, 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.book_ticker_calls, 1);
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
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(client.diagnostics().stats().simdjson_padding_fallback_messages,
            0U);
}
