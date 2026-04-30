#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

#include "core/websocket/message_view.h"
#include "exchange/gate/market_data/session.h"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"

namespace {

template <typename T>
void WriteLittleEndian(std::array<char, 192>& buffer, size_t offset,
                       T value) noexcept {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

void WriteVarString(std::array<char, 192>& buffer, size_t* offset,
                    std::string_view value) noexcept {
  buffer[(*offset)++] = static_cast<char>(value.size());
  std::memcpy(buffer.data() + *offset, value.data(), value.size());
  *offset += value.size();
}

std::string_view BuildBookTickerPayload(std::array<char, 192>* buffer,
                                        std::string_view symbol) {
  size_t offset = 0;
  WriteLittleEndian<std::uint16_t>(*buffer, offset, 59);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset,
                                   aquila::gate::kGateSbeBookTickerTemplateId);
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

using Session = aquila::gate::FuturesMarketDataSession<RecordingConsumer>;

Session MakeSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "443";
  config.target = "/v4/ws/usdt/sbe?sbe_schema_id=1";
  return Session(std::move(config), symbols, consumer);
}

}  // namespace

TEST(GateFuturesMarketDataSessionTest, MarksSubscribeAckSubscribed) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(
      R"({"time":1,"channel":"futures.book_ticker","event":"subscribe","result":{"status":"success"}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribed);
  EXPECT_EQ(session.stats().text_messages, 1U);
  EXPECT_EQ(session.stats().control_messages, 1U);
  EXPECT_EQ(session.stats().subscribe_acks, 1U);
}

TEST(GateFuturesMarketDataSessionTest, SendsSubscribeWhenActive) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kActive);

  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribeSent);
  EXPECT_EQ(session.subscribe_status(), aquila::websocket::SendStatus::kOk);
  EXPECT_EQ(session.stats().subscribe_sent, 1U);
}

TEST(GateFuturesMarketDataSessionTest, MarksUnsubscribeAckUnsubscribed) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(
      R"({"time":1,"channel":"futures.book_ticker","event":"unsubscribe","result":{"status":"success"}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kUnsubscribed);
  EXPECT_EQ(session.stats().unsubscribe_acks, 1U);
}

TEST(GateFuturesMarketDataSessionTest, RecordsControlErrorAsRejected) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(
      R"({"time":1,"channel":"futures.book_ticker","event":"subscribe","error":{"label":"INVALID_PARAM","message":"bad"}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kRejected);
  EXPECT_EQ(session.stats().control_errors, 1U);
}

TEST(GateFuturesMarketDataSessionTest, MalformedTextIsAcceptedAndCounted) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView("{"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().text_messages, 1U);
  EXPECT_EQ(session.stats().control_parse_errors, 1U);
}

TEST(GateFuturesMarketDataSessionTest, JsonUpdateRoutesToUnsupportedCounter) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(
      R"({"time":1,"channel":"futures.some_json_only_channel","event":"update","result":{"x":1}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().json_market_data_messages, 1U);
  EXPECT_EQ(session.stats().unsupported_json_market_data_messages, 1U);
  EXPECT_EQ(consumer.calls, 0);
}

TEST(GateFuturesMarketDataSessionTest, DelegatesBinaryBookTickerToClient) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);
  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = session.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().binary_messages, 1U);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.id, 42);
}
