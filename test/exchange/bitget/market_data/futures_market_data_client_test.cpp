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
#include "evaluation/exchange/bitget/sbe/book_ticker_payload_builder.h"
#include "evaluation/exchange/bitget/sbe/trade_payload_builder.h"
#include "exchange/bitget/market_data/client.h"
#include "exchange/bitget/market_data/subscription.h"

namespace {

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

using DefaultClient =
    aquila::bitget::FuturesMarketDataClient<RecordingConsumer>;
using DiagnosticClient = aquila::bitget::FuturesMarketDataClient<
    RecordingConsumer, aquila::bitget::FuturesMarketDataDiagnostics>;
using CoarseClockClient = aquila::bitget::FuturesMarketDataClient<
    RecordingConsumer, aquila::bitget::NoopFuturesMarketDataDiagnostics,
    CoarseClockOptions>;

static_assert(!DefaultClient::DiagnosticsEnabled);
static_assert(DiagnosticClient::DiagnosticsEnabled);
static_assert(DefaultClient::kClockSource ==
              aquila::websocket::DefaultWebSocketOptions::kClockSource);
static_assert(CoarseClockClient::kClockSource ==
              aquila::websocket::ClockSource::kMonotonicCoarse);
static_assert(!std::is_constructible_v<
              DefaultClient, std::span<const aquila::bitget::SymbolBinding>,
              RecordingConsumer&, aquila::websocket::ClockSource>);

}  // namespace

TEST(BitgetFuturesMarketDataClientTest, BuildsBooks1SubscribeRequest) {
  const std::array<std::string_view, 2> symbols{"BTCUSDT", "ETHUSDT"};

  const std::string request =
      aquila::bitget::BuildBooks1SubscribeRequest("usdt-futures", symbols);

  EXPECT_EQ(
      request,
      R"({"op":"subscribe","args":[{"instType":"usdt-futures","topic":"books1","symbol":"BTCUSDT"},{"instType":"usdt-futures","topic":"books1","symbol":"ETHUSDT"}]})");
}

TEST(BitgetFuturesMarketDataClientTest, BuildsBooks1UnsubscribeRequest) {
  const std::array<std::string_view, 1> symbols{"BTCUSDT"};

  const std::string request =
      aquila::bitget::BuildBooks1UnsubscribeRequest("usdt-futures", symbols);

  EXPECT_EQ(
      request,
      R"({"op":"unsubscribe","args":[{"instType":"usdt-futures","topic":"books1","symbol":"BTCUSDT"}]})");
}

TEST(BitgetFuturesMarketDataClientTest, BuildsPublicTradeSubscribeRequest) {
  const std::array<std::string_view, 2> symbols{"BTCUSDT", "ETHUSDT"};

  const std::string request =
      aquila::bitget::BuildPublicTradeSubscribeRequest("usdt-futures", symbols);

  EXPECT_EQ(
      request,
      R"({"op":"subscribe","args":[{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},{"instType":"usdt-futures","topic":"publicTrade","symbol":"ETHUSDT"}]})");
}

TEST(BitgetFuturesMarketDataClientTest, BuildsPublicTradeUnsubscribeRequest) {
  const std::array<std::string_view, 1> symbols{"BTCUSDT"};

  const std::string request =
      aquila::bitget::BuildPublicTradeUnsubscribeRequest("usdt-futures",
                                                         symbols);

  EXPECT_EQ(
      request,
      R"({"op":"unsubscribe","args":[{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"}]})");
}

TEST(BitgetFuturesMarketDataClientTest,
     EmitsBookTickerFromBinaryBooks1Payload) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::bitget::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(&buffer, "BTCUSDT");

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(consumer.last.exchange, aquila::Exchange::kBitget);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.id, 42);
  EXPECT_EQ(consumer.last.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last.bid_price, 65'690.38);
  EXPECT_DOUBLE_EQ(consumer.last.bid_volume, 1.5);
  EXPECT_DOUBLE_EQ(consumer.last.ask_price, 65'690.42);
  EXPECT_DOUBLE_EQ(consumer.last.ask_volume, 2.0);
}

TEST(BitgetFuturesMarketDataClientTest,
     EmitsTradesFromBinaryPublicTradePayload) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  std::array<char, 256> buffer{};
  const aquila::bitget::evaluation::PublicTradePayloadEntry entry{
      .ts = 1'700'000'000'000'002,
      .exec_id = 9999,
      .price = 6'566'738,
      .size = 5'000,
      .side = 1,
  };
  const std::string_view payload =
      aquila::bitget::evaluation::BuildPublicTradePayload(
          &buffer, "BTCUSDT", std::span<const decltype(entry)>(&entry, 1));

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
  ASSERT_EQ(consumer.trade_calls, 1);
  EXPECT_EQ(consumer.last_trade.symbol_id, 11);
  EXPECT_EQ(consumer.last_trade.exchange, aquila::Exchange::kBitget);
  EXPECT_EQ(consumer.last_trade.id, 9999);
  EXPECT_EQ(consumer.last_trade.side, aquila::OrderSide::kSell);
  EXPECT_EQ(consumer.last_trade.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last_trade.price, 65'667.38);
  EXPECT_DOUBLE_EQ(consumer.last_trade.volume, 0.5);
}

TEST(BitgetFuturesMarketDataClientTest,
     EmplacesTradesWhenConsumerSupportsSlotWriter) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  EmplaceOnlyConsumer consumer;
  aquila::bitget::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 256> buffer{};
  const aquila::bitget::evaluation::PublicTradePayloadEntry entry{
      .ts = 1'700'000'000'000'002,
      .exec_id = 9999,
      .price = 6'566'738,
      .size = 5'000,
      .side = 0,
  };
  const std::string_view payload =
      aquila::bitget::evaluation::BuildPublicTradePayload(
          &buffer, "BTCUSDT", std::span<const decltype(entry)>(&entry, 1));

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.emplace_calls, 0);
  ASSERT_EQ(consumer.trade_emplace_calls, 1);
  EXPECT_EQ(consumer.last_trade.id, 9999);
  EXPECT_EQ(consumer.last_trade.symbol_id, 11);
  EXPECT_EQ(consumer.last_trade.side, aquila::OrderSide::kBuy);
}

TEST(BitgetFuturesMarketDataClientTest,
     EmplacesBookTickerWhenConsumerSupportsSlotWriter) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  EmplaceOnlyConsumer consumer;
  aquila::bitget::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(&buffer, "BTCUSDT");

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.emplace_calls, 1);
  EXPECT_EQ(consumer.last.exchange, aquila::Exchange::kBitget);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.id, 42);
  EXPECT_EQ(consumer.last.local_ns, 999'000);
}

TEST(BitgetFuturesMarketDataClientTest, ExposesWebSocketMessageCallback) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(&buffer, "BTCUSDT");
  const aquila::websocket::MessageCallback message_callback =
      client.AsMessageCallback();

  const auto result = message_callback.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_GT(consumer.last.local_ns, 0);
}

TEST(BitgetFuturesMarketDataClientTest, BindsAsTypedWebSocketHandler) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::bitget::FuturesMarketDataClient client(symbols, consumer);
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

TEST(BitgetFuturesMarketDataClientTest, IgnoresUnknownTemplate) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(
          &buffer, "BTCUSDT",
          aquila::bitget::evaluation::BookTickerPayloadOptions{
              .template_id = 9999,
          });

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
  EXPECT_EQ(client.diagnostics().stats().unsupported_sbe_templates, 1U);
}

TEST(BitgetFuturesMarketDataClientTest, RecordsPublicTradeDiagnostic) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  std::array<char, 256> buffer{};
  const aquila::bitget::evaluation::PublicTradePayloadEntry entry{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildPublicTradePayload(
          &buffer, "BTCUSDT", std::span<const decltype(entry)>(&entry, 1));

  EXPECT_EQ(client.OnMessage(BinaryView(payload), 999'000),
            aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(client.diagnostics().stats().public_trades, 1U);
}

TEST(BitgetFuturesMarketDataClientTest, AcceptsSubscribeAckText) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::bitget::FuturesMarketDataClient client(symbols, consumer);

  const auto result = client.OnMessage(
      TextView(R"({"event":"subscribe","arg":{"topic":"books1"},"code":"0"})"),
      999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
}

TEST(BitgetFuturesMarketDataClientTest, RecordsUnsupportedTemplateDiagnostic) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  RecordingConsumer consumer;
  DiagnosticClient client(symbols, consumer);

  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(
          &buffer, "BTCUSDT",
          aquila::bitget::evaluation::BookTickerPayloadOptions{
              .template_id = 9999,
          });

  EXPECT_EQ(client.OnMessage(BinaryView(payload), 999'000),
            aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(client.diagnostics().stats().unsupported_sbe_templates, 1U);
}
