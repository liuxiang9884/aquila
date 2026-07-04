#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "core/websocket/message_view.h"
#include "core/websocket/websocket_client.h"
#include "evaluation/exchange/gate/sbe/book_ticker_payload_builder.h"
#include "evaluation/exchange/gate/sbe/trade_payload_builder.h"
#include "exchange/gate/market_data/client.h"
#include "exchange/gate/market_data/subscription.h"

namespace {

using aquila::gate::evaluation::BuildBookTickerPayload;

aquila::websocket::MessageView BinaryView(std::string_view payload) noexcept {
  return {
      .kind = aquila::websocket::PayloadKind::kBinary,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 7,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

aquila::websocket::MessageView TextView(std::string_view payload) noexcept {
  return {
      .kind = aquila::websocket::PayloadKind::kText,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 8,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

struct RecordingConsumer {
  int book_ticker_calls{0};
  int trade_calls{0};
  aquila::BookTicker last{};
  aquila::Trade last_trade{};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++book_ticker_calls;
    last = book_ticker;
  }

  void OnTrade(const aquila::Trade& trade) noexcept {
    ++trade_calls;
    last_trade = trade;
  }
};

struct EmplaceOnlyConsumer {
  int emplace_calls{0};
  int trade_emplace_calls{0};
  aquila::BookTicker last{};
  aquila::Trade last_trade{};

  void OnBookTicker(const aquila::BookTicker&) noexcept = delete;
  void OnTrade(const aquila::Trade&) noexcept = delete;

  template <typename Writer>
  void EmplaceBookTickerWith(Writer&& writer) noexcept {
    ++emplace_calls;
    writer(last);
  }

  template <typename Writer>
  void EmplaceTradeWith(Writer&& writer) noexcept {
    ++trade_emplace_calls;
    writer(last_trade);
  }
};

struct CoarseClockOptions : aquila::websocket::DefaultWebSocketOptions {
  static constexpr aquila::websocket::ClockSource kClockSource =
      aquila::websocket::ClockSource::kMonotonicCoarse;
};

using DefaultClient = aquila::gate::FuturesMarketDataClient<RecordingConsumer>;
using DiagnosticClient = aquila::gate::FuturesMarketDataClient<
    RecordingConsumer, aquila::gate::FuturesMarketDataDiagnostics>;
using CoarseClockClient = aquila::gate::FuturesMarketDataClient<
    RecordingConsumer, aquila::gate::NoopFuturesMarketDataDiagnostics,
    CoarseClockOptions>;

static_assert(!DefaultClient::DiagnosticsEnabled);
static_assert(DiagnosticClient::DiagnosticsEnabled);
static_assert(DefaultClient::kClockSource ==
              aquila::websocket::DefaultWebSocketOptions::kClockSource);
static_assert(CoarseClockClient::kClockSource ==
              aquila::websocket::ClockSource::kMonotonicCoarse);
static_assert(!std::is_constructible_v<
              DefaultClient, std::span<const aquila::gate::SymbolBinding>,
              RecordingConsumer&, aquila::websocket::ClockSource>);

}  // namespace

TEST(GateFuturesMarketDataClientTest, BuildsBookTickerSubscribeRequest) {
  const std::array<std::string_view, 2> symbols{"BTC_USDT", "ETH_USDT"};

  const std::string request =
      aquila::gate::BuildFuturesBookTickerSubscribeRequest(symbols, 123);

  EXPECT_EQ(
      request,
      R"({"time":123,"channel":"futures.book_ticker","event":"subscribe","payload":["BTC_USDT","ETH_USDT"]})");
}

TEST(GateFuturesMarketDataClientTest, BuildsBookTickerUnsubscribeRequest) {
  const std::array<std::string_view, 1> symbols{"BTC_USDT"};

  const std::string request =
      aquila::gate::BuildFuturesBookTickerUnsubscribeRequest(symbols, 123);

  EXPECT_EQ(
      request,
      R"({"time":123,"channel":"futures.book_ticker","event":"unsubscribe","payload":["BTC_USDT"]})");
}

TEST(GateFuturesMarketDataClientTest, BuildsTradeSubscribeRequest) {
  const std::array<std::string_view, 2> symbols{"BTC_USDT", "ETH_USDT"};

  const std::string request =
      aquila::gate::BuildFuturesTradeSubscribeRequest(symbols, 123);

  EXPECT_EQ(
      request,
      R"({"time":123,"channel":"futures.trades","event":"subscribe","payload":["BTC_USDT","ETH_USDT"]})");
}

TEST(GateFuturesMarketDataClientTest, EmitsBookTickerFromBinaryBboPayload) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(consumer.last.id, 42);
  EXPECT_EQ(consumer.last.exchange_ns, 1'770'000'000'001'000'000);
  EXPECT_EQ(consumer.last.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last.bid_price, 65'012.0);
  EXPECT_DOUBLE_EQ(consumer.last.bid_volume, 21.0);
  EXPECT_DOUBLE_EQ(consumer.last.ask_price, 65'012.5);
  EXPECT_DOUBLE_EQ(consumer.last.ask_volume, 17.5);
}

TEST(GateFuturesMarketDataClientTest, EmitsTradesFromBinaryPublicTradePayload) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  std::array<char, 256> buffer{};
  const aquila::gate::evaluation::PublicTradePayloadEntry entry{
      .t = 1'770'000'000'000'990,
      .id = 123456789,
      .size = -21'000,
      .price = 650'120'000,
  };
  const std::string_view payload =
      aquila::gate::evaluation::BuildPublicTradePayload(
          &buffer, "BTC_USDT", std::span<const decltype(entry)>(&entry, 1));

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
  ASSERT_EQ(consumer.trade_calls, 1);
  EXPECT_EQ(consumer.last_trade.symbol_id, 11);
  EXPECT_EQ(consumer.last_trade.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(consumer.last_trade.id, 123456789);
  EXPECT_EQ(consumer.last_trade.side, aquila::OrderSide::kSell);
  EXPECT_EQ(consumer.last_trade.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last_trade.price, 65'012.0);
  EXPECT_DOUBLE_EQ(consumer.last_trade.volume, 21.0);
}

TEST(GateFuturesMarketDataClientTest,
     EmplacesTradesWhenConsumerSupportsSlotWriter) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  EmplaceOnlyConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 256> buffer{};
  const aquila::gate::evaluation::PublicTradePayloadEntry entry{
      .t = 1'770'000'000'000'990,
      .id = 123456789,
      .size = 21'000,
      .price = 650'120'000,
  };
  const std::string_view payload =
      aquila::gate::evaluation::BuildPublicTradePayload(
          &buffer, "BTC_USDT", std::span<const decltype(entry)>(&entry, 1));

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.emplace_calls, 0);
  ASSERT_EQ(consumer.trade_emplace_calls, 1);
  EXPECT_EQ(consumer.last_trade.id, 123456789);
  EXPECT_EQ(consumer.last_trade.symbol_id, 11);
  EXPECT_EQ(consumer.last_trade.side, aquila::OrderSide::kBuy);
}

TEST(GateFuturesMarketDataClientTest,
     EmplacesBookTickerWhenConsumerSupportsSlotWriter) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  EmplaceOnlyConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.emplace_calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(consumer.last.id, 42);
  EXPECT_EQ(consumer.last.exchange_ns, 1'770'000'000'001'000'000);
  EXPECT_EQ(consumer.last.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last.bid_price, 65'012.0);
  EXPECT_DOUBLE_EQ(consumer.last.bid_volume, 21.0);
  EXPECT_DOUBLE_EQ(consumer.last.ask_price, 65'012.5);
  EXPECT_DOUBLE_EQ(consumer.last.ask_volume, 17.5);
}

TEST(GateFuturesMarketDataClientTest, ExposesWebSocketMessageCallback) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");
  const aquila::websocket::MessageCallback message_callback =
      client.AsMessageCallback();

  const auto result = message_callback.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_GT(consumer.last.local_ns, 0);
}

TEST(GateFuturesMarketDataClientTest, HandlesWebSocketMessageDirectly) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = client.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_GT(consumer.last.local_ns, 0);
}

TEST(GateFuturesMarketDataClientTest, HandlesBinaryPayloadDirectly) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = client.OnBinaryPayload(
      std::as_bytes(std::span(payload.data(), payload.size())), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.local_ns, 999'000);
}

TEST(GateFuturesMarketDataClientTest, MapsMultipleSymbolsByExchangeSymbol) {
  const std::array<aquila::gate::SymbolBinding, 3> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11},
      aquila::gate::SymbolBinding{.exchange_symbol = "ETH_USDT",
                                  .symbol_id = 12},
      aquila::gate::SymbolBinding{.exchange_symbol = "SOL_USDT",
                                  .symbol_id = 13}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "SOL_USDT");

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 13);
}

TEST(GateFuturesMarketDataClientTest, BindsAsTypedWebSocketHandler) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);
  auto handler = aquila::websocket::MakeMessageHandler(client);

  using Handler = decltype(handler);
  using Client = aquila::websocket::PlainWebSocketClientWithHandler<Handler>;

  static_assert(
      std::is_constructible_v<Client, aquila::websocket::ConnectionConfig,
                              Handler>);
  EXPECT_FALSE(Client::TransportUsesTls);
  EXPECT_EQ(handler.Handle(TextView("ignored")),
            aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
}

TEST(GateFuturesMarketDataClientTest, IgnoresUnknownTemplate) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload =
      BuildBookTickerPayload(&buffer, "BTC_USDT", 99);

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
  EXPECT_EQ(client.diagnostics().stats().unsupported_sbe_templates, 1U);
}

TEST(GateFuturesMarketDataClientTest,
     AssertsInvalidBookTickerBlockLengthInDebug) {
#ifndef NDEBUG
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload =
      BuildBookTickerPayload(&buffer, "BTC_USDT", 1, 58);

  EXPECT_DEATH((void)client.OnMessage(BinaryView(payload), 999'000), "");
#else
  GTEST_SKIP() << "Invalid book ticker SBE header is a debug assert contract.";
#endif
}

TEST(GateFuturesMarketDataClientTest, AssertsNonUpdateBookTickerEventInDebug) {
#ifndef NDEBUG
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");
  buffer[aquila::gate::kSbeMessageHeaderBytes + sizeof(std::int64_t)] =
      static_cast<char>(::gate::types::Event::Subscribe);

  EXPECT_DEATH((void)client.OnMessage(BinaryView(payload), 999'000), "");
#else
  GTEST_SKIP() << "Gate BBO non-update events are a debug assert contract.";
#endif
}

TEST(GateFuturesMarketDataClientTest, AssertsUnknownSymbolInDebug) {
#ifndef NDEBUG
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "ETH_USDT",
                                  .symbol_id = 12}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  EXPECT_DEATH((void)client.OnMessage(BinaryView(payload), 999'000), "");
#else
  GTEST_SKIP() << "Unknown symbol is a debug assert contract.";
#endif
}

TEST(GateFuturesMarketDataClientTest, AcceptsTextControlMessages) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  const auto result = client.OnMessage(
      TextView(R"({"channel":"futures.book_ticker","event":"subscribe"})"),
      999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
}
