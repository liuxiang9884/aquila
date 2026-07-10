#include "exchange/bitget/trading/order_feedback_parser.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/trading/order_feedback_event.h"
#include <simdjson.h>

namespace aquila::bitget {
namespace {

constexpr std::int64_t kLocalReceiveNs = 1'750'034'397'080'123'456LL;
constexpr std::uint64_t kLocalOrderId = 72'057'594'037'927'978ULL;

struct ParseOutput {
  OrderFeedbackParseResult result;
  OrderFeedbackParserStats stats;
  std::vector<OrderFeedbackEvent> events;
};

ParseOutput Parse(std::string_view payload) {
  ParseOutput output;
  simdjson::ondemand::parser parser;
  output.result = ParseBitgetOrderFeedbackMessage(
      payload, /*readable_tail_bytes=*/0, kLocalReceiveNs, parser, output.stats,
      [&output](const OrderFeedbackEvent& event) noexcept {
        output.events.push_back(event);
        return true;
      });
  return output;
}

constexpr std::string_view kAccepted = R"({
  "action":"snapshot",
  "arg":{"instType":"UTA","topic":"order"},
  "data":[{"category":"usdt-futures","orderId":"9988",
    "clientOid":"a-72057594037927978","qty":"1.5",
    "holdMode":"one_way_mode","marginMode":"crossed",
    "cumExecQty":"0","avgPrice":"0","orderStatus":"new",
    "updatedTime":"1750034397076"}],"ts":1750034397080})";

std::string OrderPayload(std::string_view status, std::string_view quantity,
                         std::string_view cumulative,
                         std::string_view average_price,
                         std::string_view cancel_reason = {}) {
  const std::string cancel_reason_field =
      cancel_reason.empty()
          ? std::string{}
          : fmt::format(R"(,"cancelReason":"{}")", cancel_reason);
  return fmt::format(
      R"({{"action":"snapshot","arg":{{"instType":"UTA","topic":"order"}},"data":[{{"category":"usdt-futures","orderId":"9988","clientOid":"a-72057594037927978","qty":"{}","holdMode":"one_way_mode","marginMode":"crossed","cumExecQty":"{}","avgPrice":"{}","orderStatus":"{}","updatedTime":"1750034397076"{}}}]}})",
      quantity, cumulative, average_price, status, cancel_reason_field);
}

void Replace(std::string* text, std::string_view before,
             std::string_view after) {
  const std::size_t position = text->find(before);
  ASSERT_NE(position, std::string::npos);
  text->replace(position, before.size(), after);
}

TEST(BitgetOrderFeedbackParserTest, MapsAcceptedAquilaOrder) {
  const ParseOutput output = Parse(kAccepted);

  EXPECT_EQ(output.result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_FALSE(output.result.continuity_lost);
  EXPECT_EQ(output.result.orders_seen, 1U);
  EXPECT_EQ(output.result.events_emitted, 1U);
  ASSERT_EQ(output.events.size(), 1U);
  const OrderFeedbackEvent& event = output.events[0];
  EXPECT_EQ(event.kind, OrderFeedbackKind::kAccepted);
  EXPECT_EQ(event.local_order_id, kLocalOrderId);
  EXPECT_EQ(event.exchange_order_id, 9988U);
  EXPECT_DOUBLE_EQ(event.cumulative_filled_quantity, 0.0);
  EXPECT_DOUBLE_EQ(event.left_quantity, 1.5);
  EXPECT_DOUBLE_EQ(event.cancelled_quantity, 0.0);
  EXPECT_DOUBLE_EQ(event.fill_price, 0.0);
  EXPECT_EQ(event.role, OrderRole::kNone);
  EXPECT_EQ(event.finish_reason, OrderFinishReason::kUnknown);
  EXPECT_EQ(event.exchange_update_ns, 1'750'034'397'076'000'000LL);
  EXPECT_EQ(event.local_receive_ns, kLocalReceiveNs);
}

TEST(BitgetOrderFeedbackParserTest, RejectsUnrecoverableEnvelopeShapes) {
  for (
      const std::string_view payload : {
          std::string_view{"{"},
          std::string_view{R"([])"},
          std::string_view{
              R"({"action":"update","arg":{"instType":"UTA","topic":"order"},"data":[]})"},
          std::string_view{
              R"({"action":"snapshot","arg":{"instType":"USDT-FUTURES","topic":"order"},"data":[]})"},
          std::string_view{
              R"({"action":"snapshot","arg":{"instType":"UTA","topic":"fill"},"data":[]})"},
          std::string_view{
              R"({"action":"snapshot","arg":{"instType":"UTA","topic":"order"},"data":{}})"},
      }) {
    const ParseOutput output = Parse(payload);
    EXPECT_TRUE(output.result.continuity_lost) << payload;
    EXPECT_TRUE(output.events.empty()) << payload;
  }
}

TEST(BitgetOrderFeedbackParserTest, ClassifiesMalformedJson) {
  const ParseOutput output = Parse(R"({"action":"snapshot")");

  EXPECT_EQ(output.result.status, OrderFeedbackParseStatus::kInvalidJson);
  EXPECT_TRUE(output.result.continuity_lost);
  EXPECT_EQ(output.stats.invalid_json_count, 1U);
  EXPECT_EQ(output.stats.unexpected_envelope_count, 0U);
}

TEST(BitgetOrderFeedbackParserTest, IgnoresForeignAndUnroutableOrders) {
  const ParseOutput output = Parse(R"({
    "action":"snapshot","arg":{"instType":"UTA","topic":"order"},
    "data":[{"clientOid":"manual-42"},{"clientOid":""},{}]})");

  EXPECT_EQ(output.result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_FALSE(output.result.continuity_lost);
  EXPECT_EQ(output.result.orders_seen, 3U);
  EXPECT_EQ(output.result.events_emitted, 0U);
  EXPECT_TRUE(output.events.empty());
  EXPECT_EQ(output.stats.foreign_orders_ignored, 1U);
  EXPECT_EQ(output.stats.unroutable_orders_ignored, 2U);
}

TEST(BitgetOrderFeedbackParserTest,
     IgnoresNonStringClientOidBeforeAquilaValidation) {
  const ParseOutput output = Parse(R"({
    "action":"snapshot","arg":{"instType":"UTA","topic":"order"},
    "data":[{"clientOid":null},{"clientOid":123}]})");

  EXPECT_EQ(output.result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_FALSE(output.result.continuity_lost);
  EXPECT_EQ(output.result.orders_seen, 2U);
  EXPECT_EQ(output.result.events_emitted, 0U);
  EXPECT_TRUE(output.events.empty());
  EXPECT_EQ(output.stats.unroutable_orders_ignored, 2U);
}

TEST(BitgetOrderFeedbackParserTest, MalformedAquilaClientOidLosesContinuity) {
  const ParseOutput output = Parse(R"({
    "action":"snapshot","arg":{"instType":"UTA","topic":"order"},
    "data":[{"clientOid":"a-not-a-number"}]})");

  EXPECT_EQ(output.result.status,
            OrderFeedbackParseStatus::kDecodeUnrecoverable);
  EXPECT_TRUE(output.result.continuity_lost);
  EXPECT_TRUE(output.events.empty());
  EXPECT_EQ(output.stats.validation_errors, 1U);
}

TEST(BitgetOrderFeedbackParserTest, AcceptsMaximumUint64ClientOid) {
  const ParseOutput output = Parse(R"({
    "action":"snapshot","arg":{"instType":"UTA","topic":"order"},
    "data":[{"category":"usdt-futures","orderId":"9988",
      "clientOid":"a-18446744073709551615","qty":"1",
      "holdMode":"one_way_mode","marginMode":"crossed",
      "cumExecQty":"0","avgPrice":"0","orderStatus":"new",
      "updatedTime":"1750034397076"}]})");

  ASSERT_EQ(output.events.size(), 1U);
  EXPECT_EQ(output.events[0].local_order_id,
            std::numeric_limits<std::uint64_t>::max());
}

TEST(BitgetOrderFeedbackParserTest, MapsLifecycleStatusesAndQuantities) {
  struct Case {
    std::string_view status;
    std::string_view quantity;
    std::string_view cumulative;
    std::string_view average_price;
    OrderFeedbackKind expected_kind;
    double expected_left;
    double expected_cancelled;
  };
  for (const Case& test_case : {
           Case{"partially_filled", "1.5", "0.4", "100.25",
                OrderFeedbackKind::kPartialFilled, 1.1, 0.0},
           Case{"filled", "1.5", "1.5000000000005", "100.25",
                OrderFeedbackKind::kFilled, 0.0, 0.0},
           Case{"cancelled", "1.5", "0.4", "100.25",
                OrderFeedbackKind::kCancelled, 1.1, 1.1},
           Case{"canceled", "1.5", "0", "0", OrderFeedbackKind::kCancelled, 1.5,
                1.5},
       }) {
    const ParseOutput output =
        Parse(OrderPayload(test_case.status, test_case.quantity,
                           test_case.cumulative, test_case.average_price));
    ASSERT_EQ(output.result.status, OrderFeedbackParseStatus::kOk)
        << test_case.status;
    ASSERT_EQ(output.events.size(), 1U) << test_case.status;
    const OrderFeedbackEvent& event = output.events[0];
    EXPECT_EQ(event.kind, test_case.expected_kind) << test_case.status;
    EXPECT_NEAR(event.left_quantity, test_case.expected_left, 1e-12)
        << test_case.status;
    EXPECT_NEAR(event.cancelled_quantity, test_case.expected_cancelled, 1e-12)
        << test_case.status;
    EXPECT_DOUBLE_EQ(event.fill_price,
                     test_case.average_price == "0" ? 0.0 : 100.25);
    EXPECT_EQ(event.role, OrderRole::kNone);
    EXPECT_NE(event.kind, OrderFeedbackKind::kRejected);
    EXPECT_EQ(output.stats.legacy_canceled_statuses,
              test_case.status == "canceled" ? 1U : 0U);
  }
}

TEST(BitgetOrderFeedbackParserTest, MapsCancelReasonAllowlist) {
  for (const auto& [cancel_reason, expected] : {
           std::pair{"normal_cancel", OrderFinishReason::kManualCancelled},
           std::pair{"ioc_not_full_cancel",
                     OrderFinishReason::kImmediateOrCancel},
           std::pair{"self_trade_cancel",
                     OrderFinishReason::kSelfTradePrevention},
           std::pair{"stp_cancel", OrderFinishReason::kSelfTradePrevention},
           std::pair{"adl_cancel", OrderFinishReason::kAutoDeleveraging},
           std::pair{"burst_cancel", OrderFinishReason::kLiquidated},
           std::pair{"penetrate_cancel", OrderFinishReason::kLiquidated},
           std::pair{"slippage_cancel", OrderFinishReason::kUnknown},
       }) {
    const ParseOutput output =
        Parse(OrderPayload("cancelled", "2", "0.5", "10", cancel_reason));
    ASSERT_EQ(output.result.status, OrderFeedbackParseStatus::kOk)
        << cancel_reason;
    ASSERT_EQ(output.events.size(), 1U) << cancel_reason;
    EXPECT_EQ(output.events[0].finish_reason, expected) << cancel_reason;
    EXPECT_FALSE(output.result.continuity_lost) << cancel_reason;
  }
}

TEST(BitgetOrderFeedbackParserTest, RejectsMissingRequiredAquilaFields) {
  for (const auto& [field, replacement] : {
           std::pair{R"("category":"usdt-futures",)", ""},
           std::pair{R"("orderId":"9988",)", ""},
           std::pair{R"("qty":"1.5",)", ""},
           std::pair{R"("holdMode":"one_way_mode",)", ""},
           std::pair{R"("marginMode":"crossed",)", ""},
           std::pair{R"("cumExecQty":"0",)", ""},
           std::pair{R"("avgPrice":"0",)", ""},
           std::pair{R"("orderStatus":"new",)", ""},
           std::pair{R"("updatedTime":"1750034397076")",
                     R"("createdTime":"1750034397076")"},
       }) {
    std::string payload{kAccepted};
    Replace(&payload, field, replacement);
    const ParseOutput output = Parse(payload);
    EXPECT_EQ(output.result.status,
              OrderFeedbackParseStatus::kDecodeUnrecoverable)
        << field;
    EXPECT_TRUE(output.result.continuity_lost) << field;
    EXPECT_TRUE(output.events.empty()) << field;
  }
}

TEST(BitgetOrderFeedbackParserTest,
     RejectsInvalidScopeQuantityPriceStatusAndTimestamp) {
  std::vector<std::string> payloads;
  payloads.push_back(OrderPayload("new", "0", "0", "0"));
  payloads.push_back(OrderPayload("new", "1", "-0.1", "0"));
  payloads.push_back(OrderPayload("new", "1", "0.1", "10"));
  payloads.push_back(OrderPayload("partially_filled", "1", "0", "0"));
  payloads.push_back(OrderPayload("partially_filled", "1", "1", "10"));
  payloads.push_back(OrderPayload("partially_filled", "1", "0.5", "0"));
  payloads.push_back(OrderPayload("filled", "1", "1.1", "10"));
  payloads.push_back(OrderPayload("filled", "1", "1", "0"));
  payloads.push_back(OrderPayload("pending", "1", "0", "0"));

  for (const auto& [before, after] : {
           std::pair{R"("category":"usdt-futures")",
                     R"("category":"coin-futures")"},
           std::pair{R"("orderId":"9988")", R"("orderId":"0")"},
           std::pair{R"("holdMode":"one_way_mode")",
                     R"("holdMode":"hedge_mode")"},
           std::pair{R"("marginMode":"crossed")", R"("marginMode":"isolated")"},
           std::pair{R"("updatedTime":"1750034397076")",
                     R"("updatedTime":"0")"},
           std::pair{R"("updatedTime":"1750034397076")",
                     R"("updatedTime":"9223372036855")"},
       }) {
    std::string payload{kAccepted};
    Replace(&payload, before, after);
    payloads.push_back(std::move(payload));
  }

  for (const std::string& payload : payloads) {
    const ParseOutput output = Parse(payload);
    EXPECT_EQ(output.result.status,
              OrderFeedbackParseStatus::kDecodeUnrecoverable)
        << payload;
    EXPECT_TRUE(output.result.continuity_lost) << payload;
    EXPECT_TRUE(output.events.empty()) << payload;
  }
}

TEST(BitgetOrderFeedbackParserTest, ParsesMixedBatch) {
  const ParseOutput output = Parse(R"({
    "action":"snapshot","arg":{"instType":"UTA","topic":"order"},
    "data":[
      {"clientOid":"manual-42"},
      {"category":"usdt-futures","orderId":"9988",
       "clientOid":"a-72057594037927978","qty":"1.5",
       "holdMode":"one_way_mode","marginMode":"crossed",
       "cumExecQty":"0.4","avgPrice":"100.25",
       "orderStatus":"partially_filled","updatedTime":"1750034397076"},
      {"category":"usdt-futures","orderId":"9989",
       "clientOid":"a-72057594037927979","qty":"1.5",
       "holdMode":"one_way_mode","marginMode":"crossed",
       "cumExecQty":"1.5","avgPrice":"100.5",
       "orderStatus":"filled","updatedTime":"1750034397077"}]})");

  EXPECT_EQ(output.result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_FALSE(output.result.continuity_lost);
  EXPECT_EQ(output.result.orders_seen, 3U);
  EXPECT_EQ(output.result.events_emitted, 2U);
  ASSERT_EQ(output.events.size(), 2U);
  EXPECT_EQ(output.events[0].kind, OrderFeedbackKind::kPartialFilled);
  EXPECT_EQ(output.events[1].kind, OrderFeedbackKind::kFilled);
  EXPECT_EQ(output.stats.foreign_orders_ignored, 1U);
}

}  // namespace
}  // namespace aquila::bitget
