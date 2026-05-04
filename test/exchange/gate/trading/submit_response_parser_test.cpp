#include "exchange/gate/trading/submit_response_parser.h"

#include <algorithm>
#include <array>
#include <string_view>

#include <gtest/gtest.h>

#include <simdjson.h>

namespace aquila::gate {
namespace {

constexpr std::string_view kOrderPlaceAckEcho = R"json({
  "request_id": "request-id-1",
  "ack": true,
  "header": {
    "response_time": "1681195484268",
    "status": "200",
    "channel": "futures.order_place",
    "event": "api",
    "client_id": "::1-0x140001a2600",
    "x_in_time": 1681985856667508,
    "x_out_time": 1681985856667598,
    "conn_trace_id": "1bde5aaa0acf2f5f48edfd4392e1fa68",
    "trace_id": "e410abb5f74b4afc519e67920548838d",
    "conn_id": "5e74253e9c793974",
    "x_gate_ratelimit_requests_remain": 99,
    "x_gate_ratelimit_limit": 100,
    "x_gate_ratelimit_reset_timestamp": 1681195484268
  },
  "data": {
    "result": {
      "req_id": "request-id-1",
      "req_header": null,
      "req_param": {
        "contract": "BTC_USDT",
        "size": "10",
        "price": "31503.280000",
        "tif": "gtc",
        "text": "t-my-custom-id"
      }
    }
  }
})json";

constexpr std::string_view kOrderPlaceResult = R"json({
  "request_id": "request-id-1",
  "ack": false,
  "header": {
    "response_time": "1681195484360",
    "status": "200",
    "channel": "futures.order_place",
    "event": "api",
    "client_id": "::1-0x140001a2600"
  },
  "data": {
    "result": {
      "id": 74046511,
      "status": "open",
      "contract": "BTC_USDT",
      "size": "10",
      "price": "31503.3",
      "text": "t-my-custom-id"
    }
  }
})json";

constexpr std::string_view kOrderPlaceError = R"json({
  "request_id": "request-id-2",
  "ack": false,
  "header": {
    "response_time": "1681195484360",
    "status": "400",
    "channel": "futures.order_place",
    "event": "api"
  },
  "data": {
    "errs": {
      "label": "TOO_MANY_REQUESTS",
      "message": "Request Rate limit Exceeded"
    }
  }
})json";

TEST(GateSubmitResponseParserTest, ParsesOrderPlaceAckEcho) {
  const auto parsed = ParseGateSubmitResponse(kOrderPlaceAckEcho);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kAck);
  EXPECT_TRUE(parsed.ack);
  EXPECT_EQ(parsed.http_status, 200);
  EXPECT_TRUE(parsed.channel_is_order_place);
  EXPECT_EQ(parsed.request_id_hash, HashGateSubmitString("request-id-1"));
  EXPECT_EQ(parsed.req_id_hash, HashGateSubmitString("request-id-1"));
  EXPECT_EQ(parsed.exchange_order_id, 0U);
  EXPECT_EQ(parsed.text_hash, 0U);
  EXPECT_EQ(parsed.error_label_hash, 0U);
}

TEST(GateSubmitResponseParserTest, ParsesOrderPlaceAckEchoPaddedBuffer) {
  std::array<char, kOrderPlaceAckEcho.size() + simdjson::SIMDJSON_PADDING>
      scratch{};
  std::copy(kOrderPlaceAckEcho.begin(), kOrderPlaceAckEcho.end(),
            scratch.begin());
  simdjson::ondemand::parser parser;

  const auto parsed =
      ParseGateSubmitResponse(scratch, kOrderPlaceAckEcho.size(), parser);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kAck);
  EXPECT_TRUE(parsed.ack);
  EXPECT_EQ(parsed.http_status, 200);
  EXPECT_TRUE(parsed.channel_is_order_place);
  EXPECT_EQ(parsed.request_id_hash, HashGateSubmitString("request-id-1"));
  EXPECT_EQ(parsed.req_id_hash, HashGateSubmitString("request-id-1"));
}

TEST(GateSubmitResponseParserTest, ParsesOrderPlaceAckEchoMinimal) {
  const auto parsed = ParseGateSubmitAckMinimal(kOrderPlaceAckEcho);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kAck);
  EXPECT_TRUE(parsed.ack);
  EXPECT_EQ(parsed.request_id_hash, HashGateSubmitString("request-id-1"));
  EXPECT_EQ(parsed.http_status, 0);
  EXPECT_FALSE(parsed.channel_is_order_place);
  EXPECT_EQ(parsed.req_id_hash, 0U);
}

TEST(GateSubmitResponseParserTest, ParsesOrderPlaceAckEchoMinimalPaddedBuffer) {
  std::array<char, kOrderPlaceAckEcho.size() + simdjson::SIMDJSON_PADDING>
      scratch{};
  std::copy(kOrderPlaceAckEcho.begin(), kOrderPlaceAckEcho.end(),
            scratch.begin());
  simdjson::ondemand::parser parser;

  const auto parsed =
      ParseGateSubmitAckMinimal(scratch, kOrderPlaceAckEcho.size(), parser);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kAck);
  EXPECT_TRUE(parsed.ack);
  EXPECT_EQ(parsed.request_id_hash, HashGateSubmitString("request-id-1"));
  EXPECT_EQ(parsed.http_status, 0);
  EXPECT_FALSE(parsed.channel_is_order_place);
  EXPECT_EQ(parsed.req_id_hash, 0U);
}

TEST(GateSubmitResponseParserTest, ParsesOrderPlaceApiResult) {
  const auto parsed = ParseGateSubmitResponse(kOrderPlaceResult);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kResult);
  EXPECT_FALSE(parsed.ack);
  EXPECT_EQ(parsed.http_status, 200);
  EXPECT_EQ(parsed.exchange_order_id, 74046511U);
  EXPECT_EQ(parsed.text_hash, HashGateSubmitString("t-my-custom-id"));
  EXPECT_EQ(parsed.error_label_hash, 0U);
}

TEST(GateSubmitResponseParserTest, ParsesOrderPlaceError) {
  const auto parsed = ParseGateSubmitResponse(kOrderPlaceError);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kError);
  EXPECT_FALSE(parsed.ack);
  EXPECT_EQ(parsed.http_status, 400);
  EXPECT_EQ(parsed.error_label_hash, HashGateSubmitString("TOO_MANY_REQUESTS"));
  EXPECT_EQ(parsed.exchange_order_id, 0U);
  EXPECT_EQ(parsed.text_hash, 0U);
}

TEST(GateSubmitResponseParserTest, EmptyStringStatusParsesAsMissingStatus) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "request-id-empty-status",
    "ack": false,
    "header": {
      "status": "",
      "channel": "futures.order_place"
    },
    "data": {
      "result": {
        "id": 74046511,
        "text": "t-my-custom-id"
      }
    }
  })json";

  const auto parsed = ParseGateSubmitResponse(kPayload);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kResult);
  EXPECT_EQ(parsed.http_status, 0);
  EXPECT_EQ(parsed.exchange_order_id, 74046511U);
}

TEST(GateSubmitResponseParserTest, AssertsNullUint64OutputInDebug) {
#ifndef NDEBUG
  static constexpr std::string_view kPayload = R"({"value":123})";
  simdjson::padded_string payload(kPayload);
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  ASSERT_EQ(parser.iterate(payload).get(document), simdjson::SUCCESS);

  simdjson::ondemand::object root;
  ASSERT_EQ(document.get_object().get(root), simdjson::SUCCESS);
  simdjson::ondemand::value value;
  ASSERT_TRUE(detail::FindSimdjsonField(root, "value", &value));

  EXPECT_DEATH((void)detail::ReadSimdjsonUint64(value, nullptr), "");
#else
  GTEST_SKIP() << "Null uint64 output is a debug assert contract.";
#endif
}

}  // namespace
}  // namespace aquila::gate
