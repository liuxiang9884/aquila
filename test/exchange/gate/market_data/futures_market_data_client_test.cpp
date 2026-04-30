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
#include "exchange/gate/market_data/client.h"
#include "exchange/gate/market_data/subscription.h"
#include "exchange/gate/sbe/test_support/book_ticker_payload_builder.h"

namespace {

using aquila::gate::test_support::BuildBookTickerPayload;

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
  int calls{0};
  aquila::BookTicker last{};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++calls;
    last = book_ticker;
  }
};

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

TEST(GateFuturesMarketDataClientTest, EmitsBookTickerFromBinaryBboPayload) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(consumer.last.id, 42);
  EXPECT_EQ(consumer.last.exchange_ns, 1'770'000'000'000'900'000);
  EXPECT_EQ(consumer.last.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last.bid_price, 65'012.0);
  EXPECT_DOUBLE_EQ(consumer.last.bid_volume, 21.0);
  EXPECT_DOUBLE_EQ(consumer.last.ask_price, 65'012.5);
  EXPECT_DOUBLE_EQ(consumer.last.ask_volume, 17.5);
}

TEST(GateFuturesMarketDataClientTest, ExposesWebSocketMessageCallback) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");
  const aquila::websocket::MessageCallback message_callback =
      client.AsMessageCallback();

  const auto result = message_callback.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_GT(consumer.last.local_ns, 0);
}

TEST(GateFuturesMarketDataClientTest, HandlesWebSocketMessageDirectly) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = client.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_GT(consumer.last.local_ns, 0);
}

TEST(GateFuturesMarketDataClientTest, HandlesBinaryPayloadDirectly) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = client.OnBinaryPayload(
      std::as_bytes(std::span(payload.data(), payload.size())), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.local_ns, 999'000);
}

TEST(GateFuturesMarketDataClientTest, MapsMultipleSymbolsBySymbolText) {
  const std::array<aquila::gate::SymbolBinding, 3> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11},
      aquila::gate::SymbolBinding{.symbol = "ETH_USDT", .symbol_id = 12},
      aquila::gate::SymbolBinding{.symbol = "SOL_USDT", .symbol_id = 13}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "SOL_USDT");

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 13);
}

TEST(GateFuturesMarketDataClientTest, BindsAsTypedWebSocketHandler) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
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
  EXPECT_EQ(consumer.calls, 0);
}

TEST(GateFuturesMarketDataClientTest, IgnoresUnknownTemplate) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload =
      BuildBookTickerPayload(&buffer, "BTC_USDT", 99);

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 0);
  EXPECT_EQ(client.stats().unsupported_sbe_templates, 1U);
}

TEST(GateFuturesMarketDataClientTest, IgnoresInvalidBookTickerBlockLength) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload =
      BuildBookTickerPayload(&buffer, "BTC_USDT", 1, 58);

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 0);
  EXPECT_EQ(client.stats().book_ticker_decode_failures, 1U);
}

TEST(GateFuturesMarketDataClientTest, IgnoresUnknownSymbol) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "ETH_USDT", .symbol_id = 12}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 0);
  EXPECT_EQ(client.stats().unknown_symbols, 1U);
}

TEST(GateFuturesMarketDataClientTest, AcceptsTextControlMessages) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  const auto result = client.OnMessage(
      TextView(R"({"channel":"futures.book_ticker","event":"subscribe"})"),
      999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 0);
}
