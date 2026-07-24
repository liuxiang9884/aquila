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
constexpr std::string_view kRunNamespace = "0123456789AB";

struct OwnedOrderFeedbackDiagnosticRecord {
  std::string client_oid;
  std::string order_id;
  std::string order_status;
  std::string cancel_reason;
  OrderFeedbackDiagnosticKind kind{
      OrderFeedbackDiagnosticKind::kProtocolUpdate};
  ClientOidIgnoreReason ignore_reason{ClientOidIgnoreReason::kNone};
  std::uint64_t exchange_message_time_ms{0};
  std::uint64_t created_time_ms{0};
  std::uint64_t updated_time_ms{0};
  std::uint32_t batch_data_index{0};
};

OwnedOrderFeedbackDiagnosticRecord Own(
    const OrderFeedbackDiagnosticRecord& record) {
  return {
      .client_oid = std::string(record.client_oid),
      .order_id = std::string(record.order_id),
      .order_status = std::string(record.order_status),
      .cancel_reason = std::string(record.cancel_reason),
      .kind = record.kind,
      .ignore_reason = record.ignore_reason,
      .exchange_message_time_ms = record.exchange_message_time_ms,
      .created_time_ms = record.created_time_ms,
      .updated_time_ms = record.updated_time_ms,
      .batch_data_index = record.batch_data_index,
  };
}

struct ParseOutput {
  OrderFeedbackParseResult result;
  OrderFeedbackParserStats stats;
  std::vector<OrderFeedbackEvent> events;
  std::vector<OwnedOrderFeedbackDiagnosticRecord> diagnostic_records;
};

ParseOutput Parse(std::string_view payload) {
  ParseOutput output;
  simdjson::ondemand::parser parser;
  output.result = ParseBitgetOrderFeedbackMessage(
      payload, /*readable_tail_bytes=*/0, kLocalReceiveNs, parser, output.stats,
      ClientOidRunNamespace::Parse(kRunNamespace).value(),
      [&output](const OrderFeedbackEvent& event) noexcept {
        output.events.push_back(event);
        return true;
      },
      [&output](const OrderFeedbackDiagnosticRecord& record) noexcept {
        output.diagnostic_records.push_back(Own(record));
      });
  return output;
}

constexpr std::string_view kAccepted = R"({
  "action":"snapshot",
  "arg":{"instType":"UTA","topic":"order"},
  "data":[{"category":"usdt-futures","orderId":"9988",
    "clientOid":"a1-0123456789AB-00JPIA9PM8JSA","qty":"1.5",
    "holdMode":"one_way_mode","marginMode":"crossed",
    "cumExecQty":"0","avgPrice":"0","orderStatus":"new",
    "createdTime":"1750034397001",
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
      R"({{"action":"snapshot","arg":{{"instType":"UTA","topic":"order"}},"data":[{{"category":"usdt-futures","orderId":"9988","clientOid":"a1-0123456789AB-00JPIA9PM8JSA","qty":"{}","holdMode":"one_way_mode","marginMode":"crossed","cumExecQty":"{}","avgPrice":"{}","orderStatus":"{}","updatedTime":"1750034397076"{}}}]}})",
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
  ASSERT_EQ(output.diagnostic_records.size(), 1U);
  const OwnedOrderFeedbackDiagnosticRecord& diagnostic =
      output.diagnostic_records.front();
  EXPECT_EQ(diagnostic.client_oid, "a1-0123456789AB-00JPIA9PM8JSA");
  EXPECT_EQ(diagnostic.kind, OrderFeedbackDiagnosticKind::kProtocolUpdate);
  EXPECT_EQ(diagnostic.ignore_reason, ClientOidIgnoreReason::kNone);
  EXPECT_EQ(diagnostic.order_id, "9988");
  EXPECT_EQ(diagnostic.order_status, "new");
  EXPECT_TRUE(diagnostic.cancel_reason.empty());
  EXPECT_EQ(diagnostic.exchange_message_time_ms, 1'750'034'397'080ULL);
  EXPECT_EQ(diagnostic.created_time_ms, 1'750'034'397'001ULL);
  EXPECT_EQ(diagnostic.updated_time_ms, 1'750'034'397'076ULL);
  EXPECT_EQ(diagnostic.batch_data_index, 0U);
}

TEST(BitgetOrderFeedbackParserTest,
     ClassifiesFastFillWithoutOrderContinuityLoss) {
  const ParseOutput output = Parse(R"({
    "action":"update","arg":{"instType":"UTA","topic":"fast-fill"},
    "data":{"orderId":"9988","execId":"77"}})");

  EXPECT_EQ(output.result.status, OrderFeedbackParseStatus::kFastFillMessage);
  EXPECT_FALSE(output.result.continuity_lost);
  EXPECT_TRUE(output.events.empty());
  EXPECT_TRUE(output.diagnostic_records.empty());
}

TEST(BitgetOrderFeedbackParserTest,
     DiagnosticFieldsDoNotNarrowAuthoritativeOrderDecoding) {
  std::string payload{kAccepted};
  Replace(&payload, R"("orderId":"9988")", R"("orderId":9988)");
  Replace(&payload, R"("createdTime":"1750034397001")",
          R"("createdTime":"unavailable")");
  Replace(&payload, R"("ts":1750034397080)", R"("ts":"unavailable")");

  const ParseOutput output = Parse(payload);

  EXPECT_EQ(output.result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_FALSE(output.result.continuity_lost);
  ASSERT_EQ(output.events.size(), 1U);
  EXPECT_EQ(output.events.front().exchange_order_id, 9988U);
  ASSERT_EQ(output.diagnostic_records.size(), 1U);
  EXPECT_TRUE(output.diagnostic_records.front().order_id.empty());
  EXPECT_EQ(output.diagnostic_records.front().created_time_ms, 0U);
  EXPECT_EQ(output.diagnostic_records.front().exchange_message_time_ms, 0U);
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
    "data":[{"clientOid":"a1-0123456789AB-00000000000!1"}]})");

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
      "clientOid":"a1-0123456789AB-3W5E11264SGSF","qty":"1",
      "holdMode":"one_way_mode","marginMode":"crossed",
      "cumExecQty":"0","avgPrice":"0","orderStatus":"new",
      "updatedTime":"1750034397076"}]})");

  ASSERT_EQ(output.events.size(), 1U);
  EXPECT_EQ(output.events[0].local_order_id,
            std::numeric_limits<std::uint64_t>::max());
}

TEST(BitgetOrderFeedbackParserTest,
     IgnoresForeignRunNamespaceAndLegacyWithoutContinuityLoss) {
  const ParseOutput output = Parse(R"({
    "action":"snapshot","arg":{"instType":"UTA","topic":"order"},
    "data":[
      {"clientOid":"a1-ZYXWVTSRQPNM-00000000000!1"},
      {"clientOid":"a-72057594037927978"}]})");

  EXPECT_EQ(output.result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_FALSE(output.result.continuity_lost);
  EXPECT_TRUE(output.events.empty());
  EXPECT_EQ(output.stats.foreign_run_namespace_orders_ignored, 1U);
  EXPECT_EQ(output.stats.legacy_client_oid_orders_ignored, 1U);
  EXPECT_EQ(output.stats.validation_errors, 0U);
  ASSERT_EQ(output.diagnostic_records.size(), 2U);
  EXPECT_EQ(output.diagnostic_records[0].kind,
            OrderFeedbackDiagnosticKind::kClientOidIgnored);
  EXPECT_EQ(output.diagnostic_records[0].ignore_reason,
            ClientOidIgnoreReason::kForeignRunNamespace);
  EXPECT_EQ(output.diagnostic_records[1].ignore_reason,
            ClientOidIgnoreReason::kLegacyClientOid);
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
    ASSERT_EQ(output.diagnostic_records.size(), 1U) << cancel_reason;
    EXPECT_EQ(output.diagnostic_records[0].cancel_reason, cancel_reason);
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
       "clientOid":"a1-0123456789AB-00JPIA9PM8JSA","qty":"1.5",
       "holdMode":"one_way_mode","marginMode":"crossed",
       "cumExecQty":"0.4","avgPrice":"100.25",
       "orderStatus":"partially_filled","updatedTime":"1750034397076"},
      {"category":"usdt-futures","orderId":"9989",
       "clientOid":"a1-0123456789AB-00JPIA9PM8JSB","qty":"1.5",
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
  ASSERT_EQ(output.diagnostic_records.size(), 2U);
  EXPECT_EQ(output.diagnostic_records[0].batch_data_index, 1U);
  EXPECT_EQ(output.diagnostic_records[1].batch_data_index, 2U);
  EXPECT_EQ(output.stats.foreign_orders_ignored, 1U);
}

}  // namespace
}  // namespace aquila::bitget
