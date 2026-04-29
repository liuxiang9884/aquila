#include "exchange/gate/market_data/client.h"
#include "exchange/gate/market_data/subscription.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "core/websocket/message_view.h"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"

namespace {

template <typename T>
void WriteLittleEndian(std::array<char, 192>& buffer,
                       size_t offset,
                       T value) noexcept {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

void WriteVarString(std::array<char, 192>& buffer,
                    size_t* offset,
                    std::string_view value) noexcept {
  buffer[(*offset)++] = static_cast<char>(value.size());
  std::memcpy(buffer.data() + *offset, value.data(), value.size());
  *offset += value.size();
}

std::string_view BuildBookTickerPayload(std::array<char, 192>* buffer,
                                        std::string_view symbol,
                                        std::uint16_t template_id = 1) {
  size_t offset = 0;
  WriteLittleEndian<std::uint16_t>(*buffer, offset, 59);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, template_id);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset,
                                   aquila::gate::kGateSbeSchemaId);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset,
                                   aquila::gate::kGateSbeSchemaVersion);
  offset += sizeof(std::uint16_t);

  WriteLittleEndian<std::int64_t>(*buffer, offset, 1'770'000'000'001'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(
      *buffer, offset, static_cast<std::int8_t>(gate::types::Event::Update));
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 1'770'000'000'000'900);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 42);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(*buffer, offset, -4);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int8_t>(*buffer, offset, -3);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 650'125'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 17'500);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 650'120'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 21'000);
  offset += sizeof(std::int64_t);
  WriteVarString(*buffer, &offset, "futures.book_ticker");
  WriteVarString(*buffer, &offset, symbol);
  return {buffer->data(), offset};
}

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

  EXPECT_EQ(request,
            R"({"time":123,"channel":"futures.book_ticker","event":"subscribe","payload":["BTC_USDT","ETH_USDT"]})");
}

TEST(GateFuturesMarketDataClientTest, BuildsBookTickerUnsubscribeRequest) {
  const std::array<std::string_view, 1> symbols{"BTC_USDT"};

  const std::string request =
      aquila::gate::BuildFuturesBookTickerUnsubscribeRequest(symbols, 123);

  EXPECT_EQ(request,
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

TEST(GateFuturesMarketDataClientTest, ExposesWebSocketMessageConsumer) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::gate::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");
  const aquila::websocket::MessageConsumer message_consumer =
      client.AsMessageConsumer();

  const auto result = message_consumer.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_GT(consumer.last.local_ns, 0);
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
