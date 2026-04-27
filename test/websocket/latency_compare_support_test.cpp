#include "tools/websocket_latency_compare_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace aquila::tools {

TEST(WebSocketLatencyCompareSupportTest, ParsesGateBookTickerUpdate) {
  const std::string payload =
      R"({"time":1615366379,"time_ms":1615366379123,)"
      R"("channel":"futures.book_ticker","event":"update",)"
      R"("result":{"t":1615366379123,"u":2517661076,"s":"BTC_USDT",)"
      R"("b":"54696.6","B":37000,"a":"54696.7","A":47061}})";

  const auto parsed = TryParseGateBookTicker(payload);

  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->symbol, "BTC_USDT");
  EXPECT_EQ(parsed->update_id, 2517661076ULL);
  EXPECT_EQ(parsed->exchange_time_ms, 1615366379123ULL);
  EXPECT_EQ(parsed->MatchKey(), "BTC_USDT:2517661076");
}

TEST(WebSocketLatencyCompareSupportTest, ParsesStringUpdateId) {
  const std::string payload =
      R"({"channel":"futures.book_ticker","event":"update",)"
      R"("result":{"t":"1615366379123","u":"2517661076","s":"BTC_USDT"}})";

  const auto parsed = TryParseGateBookTicker(payload);

  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->update_id, 2517661076ULL);
  EXPECT_EQ(parsed->exchange_time_ms, 1615366379123ULL);
}

TEST(WebSocketLatencyCompareSupportTest, IgnoresSubscribeAck) {
  const std::string payload =
      R"({"time":1545405058,"channel":"futures.book_ticker",)"
      R"("event":"subscribe","error":null,"result":{"status":"success"}})";

  EXPECT_FALSE(TryParseGateBookTicker(payload).has_value());
}

TEST(WebSocketLatencyCompareSupportTest, MatchesPublicAndPrivateArrivals) {
  LatencyPairCollector collector(16);
  GateBookTickerKey key{
      .symbol = "BTC_USDT",
      .update_id = 42,
      .exchange_time_ms = 1000,
  };

  collector.Observe(EndpointSide::kPublic, key, 1'000);
  collector.Observe(EndpointSide::kPrivate, key, 900);
  const auto snapshot = collector.Snapshot();

  ASSERT_EQ(snapshot.matched.size(), 1U);
  EXPECT_EQ(snapshot.matched[0].key.MatchKey(), "BTC_USDT:42");
  EXPECT_EQ(snapshot.matched[0].public_arrival_ns, 1'000U);
  EXPECT_EQ(snapshot.matched[0].private_arrival_ns, 900U);
  EXPECT_EQ(snapshot.matched[0].private_lead_ns, 100);
  EXPECT_EQ(snapshot.pending_public, 0U);
  EXPECT_EQ(snapshot.pending_private, 0U);
}

TEST(WebSocketLatencyCompareSupportTest, BuildsGateSubscribeRequest) {
  const std::string request =
      BuildGateSubscribeRequest("futures.book_ticker", "BTC_USDT", 1234567890);

  EXPECT_EQ(request,
            R"({"time":1234567890,"channel":"futures.book_ticker",)"
            R"("event":"subscribe","payload":["BTC_USDT"]})");
}

}  // namespace aquila::tools
