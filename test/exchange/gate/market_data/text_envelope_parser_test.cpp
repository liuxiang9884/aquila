#include "exchange/gate/market_data/text_envelope_parser.h"

#include <array>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include <simdjson.h>

namespace {

TEST(GateTextEnvelopeParserTest, ParsesSubscribeAckFromPaddedView) {
  static constexpr std::string_view kPayload =
      R"({"time":1,"channel":"futures.book_ticker","event":"subscribe","result":{"status":"success"}})";
  std::array<char, kPayload.size() + simdjson::SIMDJSON_PADDING> buffer{};
  std::memcpy(buffer.data(), kPayload.data(), kPayload.size());
  simdjson::ondemand::parser parser;
  aquila::gate::detail::TextEnvelope envelope{};

  const bool parsed = aquila::gate::detail::ParseTextEnvelope(
      std::string_view(buffer.data(), kPayload.size()),
      simdjson::SIMDJSON_PADDING, parser, envelope);

  EXPECT_TRUE(parsed);
  EXPECT_EQ(envelope.event, aquila::gate::detail::TextEvent::kSubscribe);
  EXPECT_EQ(envelope.channel, aquila::gate::detail::TextChannel::kBookTicker);
  EXPECT_TRUE(envelope.result_success);
  EXPECT_FALSE(envelope.has_error);
}

TEST(GateTextEnvelopeParserTest, ParsesErrorEnvelope) {
  static constexpr std::string_view kPayload =
      R"({"time":1,"channel":"futures.book_ticker","event":"unsubscribe","error":{"label":"BAD","message":"bad"}})";
  simdjson::ondemand::parser parser;
  aquila::gate::detail::TextEnvelope envelope{};

  const bool parsed =
      aquila::gate::detail::ParseTextEnvelope(kPayload, 0, parser, envelope);

  EXPECT_TRUE(parsed);
  EXPECT_EQ(envelope.event, aquila::gate::detail::TextEvent::kUnsubscribe);
  EXPECT_EQ(envelope.channel, aquila::gate::detail::TextChannel::kBookTicker);
  EXPECT_FALSE(envelope.result_success);
  EXPECT_TRUE(envelope.has_error);
}

TEST(GateTextEnvelopeParserTest, ParsesTradeSubscribeChannel) {
  simdjson::ondemand::parser parser;
  aquila::gate::detail::TextEnvelope envelope{};
  ASSERT_TRUE(aquila::gate::detail::ParseTextEnvelope(
      R"({"time":1,"channel":"futures.trades","event":"subscribe","result":{"status":"success"}})",
      0, parser, envelope));
  EXPECT_EQ(envelope.channel, aquila::gate::detail::TextChannel::kTrade);
  EXPECT_EQ(envelope.event, aquila::gate::detail::TextEvent::kSubscribe);
  EXPECT_TRUE(envelope.result_success);
}

}  // namespace
