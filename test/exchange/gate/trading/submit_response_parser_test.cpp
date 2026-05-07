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

TEST(GateSubmitResponseParserTest, DecodesRequestIdAndOrderText) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "144115188075855881",
    "ack": false,
    "header": {
      "status": "200",
      "channel": "futures.order_place"
    },
    "data": {
      "result": {
        "id": "36028827892199865",
        "status": "open",
        "contract": "BTC_USDT",
        "size": "1",
        "price": "31503.3",
        "text": "t-12345"
      }
    }
  })json";

  const auto parsed = ParseGateSubmitResponse(kPayload);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kResult);
  EXPECT_TRUE(parsed.request_id.ok);
  EXPECT_EQ(parsed.request_id.type, OrderRequestType::kPlaceOrder);
  EXPECT_EQ(parsed.request_id.sequence, 9U);
  EXPECT_EQ(parsed.channel, kFuturesOrderPlace);
  EXPECT_TRUE(parsed.channel_is_order_place);
  EXPECT_TRUE(parsed.has_local_order_id);
  EXPECT_EQ(parsed.local_order_id, 12345);
  EXPECT_EQ(parsed.exchange_order_id, 36028827892199865U);
  EXPECT_EQ(parsed.text_hash, HashGateSubmitString("t-12345"));
}

TEST(GateSubmitResponseParserTest, OrderSessionProfileSkipsSuccessHashes) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "144115188075855881",
    "ack": false,
    "header": {
      "status": "200",
      "channel": "futures.order_place"
    },
    "data": {
      "result": {
        "id": "36028827892199865",
        "text": "t-12345"
      }
    }
  })json";
  std::array<char, kPayload.size() + simdjson::SIMDJSON_PADDING> scratch{};
  std::copy(kPayload.begin(), kPayload.end(), scratch.begin());
  simdjson::ondemand::parser parser;

  const auto parsed = ParseGateSubmitResponseForOrderSession(
      std::string_view(scratch.data(), kPayload.size()),
      simdjson::SIMDJSON_PADDING, parser);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kResult);
  EXPECT_TRUE(parsed.request_id.ok);
  EXPECT_EQ(parsed.request_id.type, OrderRequestType::kPlaceOrder);
  EXPECT_EQ(parsed.request_id.sequence, 9U);
  EXPECT_TRUE(parsed.has_local_order_id);
  EXPECT_EQ(parsed.local_order_id, 12345);
  EXPECT_EQ(parsed.exchange_order_id, 36028827892199865U);
  EXPECT_EQ(parsed.request_id_hash, 0U);
  EXPECT_EQ(parsed.req_id_hash, 0U);
  EXPECT_EQ(parsed.text_hash, 0U);
}

TEST(GateSubmitResponseParserTest, OrderSessionProfileKeepsErrorLabelHash) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "216172782113783818",
    "ack": false,
    "header": {
      "status": "400",
      "channel": "futures.order_cancel"
    },
    "data": {
      "errs": {
        "label": "ORDER_NOT_FOUND",
        "message": "order not found"
      }
    }
  })json";

  const auto parsed = ParseGateSubmitResponseForOrderSession(kPayload);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kError);
  EXPECT_TRUE(parsed.request_id.ok);
  EXPECT_EQ(parsed.request_id.type, OrderRequestType::kCancelOrder);
  EXPECT_EQ(parsed.request_id.sequence, 10U);
  EXPECT_EQ(parsed.request_id_hash, 0U);
  EXPECT_EQ(parsed.error_label_hash, HashGateSubmitString("ORDER_NOT_FOUND"));
}

TEST(GateSubmitResponseParserTest, DecodesCancelErrorRequestId) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": 216172782113783818,
    "ack": false,
    "header": {
      "status": "400",
      "channel": "futures.order_cancel"
    },
    "data": {
      "errs": {
        "label": "ORDER_NOT_FOUND",
        "message": "order not found"
      }
    }
  })json";

  const auto parsed = ParseGateSubmitResponse(kPayload);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kError);
  EXPECT_TRUE(parsed.request_id.ok);
  EXPECT_EQ(parsed.request_id.type, OrderRequestType::kCancelOrder);
  EXPECT_EQ(parsed.request_id.sequence, 10U);
  EXPECT_EQ(parsed.channel, kFuturesOrderCancel);
  EXPECT_FALSE(parsed.channel_is_order_place);
  EXPECT_EQ(parsed.error_label_hash, HashGateSubmitString("ORDER_NOT_FOUND"));
}

TEST(GateSubmitResponseParserTest, DecodesRequestIdFromPaddedViewOverload) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "144115188075855881",
    "ack": false,
    "header": {
      "status": "200",
      "channel": "futures.order_place"
    },
    "data": {
      "result": {
        "id": 36028827892199865,
        "text": "t-12345"
      }
    }
  })json";
  std::array<char, kPayload.size() + simdjson::SIMDJSON_PADDING> scratch{};
  std::copy(kPayload.begin(), kPayload.end(), scratch.begin());
  simdjson::ondemand::parser parser;

  const auto parsed =
      ParseGateSubmitResponse(std::string_view(scratch.data(), kPayload.size()),
                              simdjson::SIMDJSON_PADDING, parser);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_TRUE(parsed.request_id.ok);
  EXPECT_EQ(parsed.request_id.type, OrderRequestType::kPlaceOrder);
  EXPECT_EQ(parsed.request_id.sequence, 9U);
  EXPECT_EQ(parsed.channel, kFuturesOrderPlace);
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

TEST(GateSubmitResponseParserTest, DecodesRequestIdInMinimalAckParser) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "144115188075855881",
    "ack": true
  })json";

  const auto parsed = ParseGateSubmitAckMinimal(kPayload);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kAck);
  EXPECT_TRUE(parsed.ack);
  EXPECT_EQ(parsed.request_id_hash, HashGateSubmitString("144115188075855881"));
  EXPECT_TRUE(parsed.request_id.ok);
  EXPECT_EQ(parsed.request_id.type, OrderRequestType::kPlaceOrder);
  EXPECT_EQ(parsed.request_id.sequence, 9U);
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

TEST(GateSubmitResponseParserTest, MissingAckKeepsResponseKindUnknown) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "144115188075855881",
    "header": {
      "status": "200",
      "channel": "futures.order_place"
    },
    "data": {
      "result": {
        "id": "36028827892199865",
        "text": "t-12345"
      }
    }
  })json";

  const auto parsed = ParseGateSubmitResponse(kPayload);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_FALSE(parsed.has_ack);
  EXPECT_FALSE(parsed.ack);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kUnknown);
  EXPECT_TRUE(parsed.request_id.ok);
  EXPECT_EQ(parsed.exchange_order_id, 0U);
  EXPECT_FALSE(parsed.has_local_order_id);
}

TEST(GateSubmitResponseParserTest, NonBoolAckKeepsResponseKindUnknown) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "144115188075855881",
    "ack": "false",
    "header": {
      "status": "200",
      "channel": "futures.order_place"
    },
    "data": {
      "result": {
        "id": "36028827892199865",
        "text": "t-12345"
      }
    }
  })json";

  const auto parsed = ParseGateSubmitResponse(kPayload);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_FALSE(parsed.has_ack);
  EXPECT_FALSE(parsed.ack);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kUnknown);
  EXPECT_EQ(parsed.exchange_order_id, 0U);
  EXPECT_FALSE(parsed.has_local_order_id);
}

TEST(GateSubmitResponseParserTest, DecodesLoginUid) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "72057594037927937",
    "ack": false,
    "header": {
      "status": "200",
      "channel": "futures.login"
    },
    "data": {
      "result": {
        "uid": "14446887"
      }
    }
  })json";

  const auto parsed = ParseGateSubmitResponse(kPayload);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kResult);
  EXPECT_TRUE(parsed.has_login_uid);
  EXPECT_EQ(parsed.login_uid, 14446887U);
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
