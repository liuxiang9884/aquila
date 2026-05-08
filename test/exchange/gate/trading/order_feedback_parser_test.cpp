#include "exchange/gate/trading/order_feedback_parser.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "core/trading/order_feedback_event.h"
#include "evaluation/exchange/gate/trading/order_feedback_payload_builder.h"

namespace aquila::gate {
namespace {

using evaluation::BuildOrderFeedbackOrdersPayload;
using evaluation::OrderFeedbackPayloadFields;

constexpr std::uint64_t kLocalOrderId =
    evaluation::kOrderFeedbackPayloadLocalOrderId;
constexpr std::uint64_t kExchangeOrderId =
    evaluation::kOrderFeedbackPayloadExchangeOrderId;
constexpr std::int64_t kLocalReceiveNs = 1'770'000'000'001'222'000;
constexpr std::int64_t kUpdateTimeUs =
    evaluation::kOrderFeedbackPayloadUpdateTimeUs;
constexpr std::int64_t kUpdateTimeNs = kUpdateTimeUs * 1000;

template <std::size_t N = 1>
struct EventCollector {
  std::array<OrderFeedbackEvent, N> events{};
  std::size_t count{0};

  void operator()(const OrderFeedbackEvent& event) noexcept {
    if (count < events.size()) {
      events[count++] = event;
    }
  }
};

OrderFeedbackPayloadFields MakeFields(std::string_view finish_as) noexcept {
  OrderFeedbackPayloadFields fields{};
  fields.finish_as = finish_as;
  fields.text = "t-216172782113783850";
  return fields;
}

template <std::size_t N = 1>
OrderFeedbackParseResult ParseOne(const OrderFeedbackPayloadFields& fields,
                                  OrderFeedbackParserStats* stats,
                                  EventCollector<N>* collector,
                                  std::string_view channel = "futures.orders") {
  std::array<char, 512> buffer{};
  const std::string_view payload =
      BuildOrderFeedbackOrdersPayload(&buffer, fields, channel);
  return ParseGateOrderFeedbackMessage(payload, kLocalReceiveNs, *stats,
                                       *collector);
}

}  // namespace

TEST(GateOrderFeedbackParserTest, MapsNewToAcceptedEvent) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  const OrderFeedbackParseResult result =
      ParseOne(MakeFields("_new"), &stats, &collector);

  ASSERT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_EQ(result.events_emitted, 1);
  ASSERT_EQ(collector.count, 1U);
  EXPECT_EQ(collector.events[0].kind, OrderFeedbackKind::kAccepted);
  EXPECT_EQ(collector.events[0].local_order_id, kLocalOrderId);
  EXPECT_EQ(collector.events[0].exchange_order_id, kExchangeOrderId);
  EXPECT_EQ(collector.events[0].exchange_update_ns, kUpdateTimeNs);
  EXPECT_EQ(collector.events[0].local_receive_ns, kLocalReceiveNs);
  EXPECT_EQ(stats.events_emitted, 1U);
}

TEST(GateOrderFeedbackParserTest, MapsUpdateWithLeftToPartialFilledEvent) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderFeedbackPayloadFields fields = MakeFields("_update");
  fields.size_mantissa = 10;
  fields.left_mantissa = 4;
  fields.fill_price_mantissa = 6'501'250;

  const OrderFeedbackParseResult result = ParseOne(fields, &stats, &collector);

  ASSERT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  ASSERT_EQ(collector.count, 1U);
  EXPECT_EQ(collector.events[0].kind, OrderFeedbackKind::kPartialFilled);
  EXPECT_EQ(collector.events[0].cumulative_filled_quantity, 6);
  EXPECT_EQ(collector.events[0].left_quantity, 4);
  EXPECT_DOUBLE_EQ(collector.events[0].fill_price, 65'012.5);
}

TEST(GateOrderFeedbackParserTest, MapsFilledWithZeroLeftToFilledEvent) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderFeedbackPayloadFields fields = MakeFields("filled");
  fields.size_mantissa = 10;
  fields.left_mantissa = 0;
  fields.role = "taker";

  const OrderFeedbackParseResult result = ParseOne(fields, &stats, &collector);

  ASSERT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  ASSERT_EQ(collector.count, 1U);
  EXPECT_EQ(collector.events[0].kind, OrderFeedbackKind::kFilled);
  EXPECT_EQ(collector.events[0].cumulative_filled_quantity, 10);
  EXPECT_EQ(collector.events[0].left_quantity, 0);
  EXPECT_EQ(collector.events[0].role, OrderRole::kTaker);
}

TEST(GateOrderFeedbackParserTest, MapsCancelledToManualCancelledEvent) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderFeedbackPayloadFields fields = MakeFields("cancelled");
  fields.size_mantissa = 10;
  fields.left_mantissa = 7;

  const OrderFeedbackParseResult result = ParseOne(fields, &stats, &collector);

  ASSERT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  ASSERT_EQ(collector.count, 1U);
  EXPECT_EQ(collector.events[0].kind, OrderFeedbackKind::kCancelled);
  EXPECT_EQ(collector.events[0].finish_reason,
            OrderFinishReason::kManualCancelled);
  EXPECT_EQ(collector.events[0].cumulative_filled_quantity, 3);
  EXPECT_EQ(collector.events[0].cancelled_quantity, 7);
}

TEST(GateOrderFeedbackParserTest, MapsTerminalFinishReasons) {
  struct Case {
    std::string_view finish_as;
    OrderFinishReason reason;
  };
  static constexpr std::array<Case, 7> kCases{{
      {"ioc", OrderFinishReason::kImmediateOrCancel},
      {"reduce_only", OrderFinishReason::kReduceOnly},
      {"reduce_out", OrderFinishReason::kReduceOut},
      {"stp", OrderFinishReason::kSelfTradePrevention},
      {"liquidated", OrderFinishReason::kLiquidated},
      {"auto_deleveraging", OrderFinishReason::kAutoDeleveraging},
      {"position_close", OrderFinishReason::kPositionClose},
  }};

  for (const Case& test_case : kCases) {
    OrderFeedbackParserStats stats{};
    EventCollector collector{};
    const OrderFeedbackParseResult result =
        ParseOne(MakeFields(test_case.finish_as), &stats, &collector);

    ASSERT_EQ(result.status, OrderFeedbackParseStatus::kOk);
    ASSERT_EQ(collector.count, 1U) << test_case.finish_as;
    EXPECT_EQ(collector.events[0].kind, OrderFeedbackKind::kCancelled);
    EXPECT_EQ(collector.events[0].finish_reason, test_case.reason)
        << test_case.finish_as;
  }
}

TEST(GateOrderFeedbackParserTest, DropsInvalidTextAndIncrementsDiagnostics) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderFeedbackPayloadFields fields = MakeFields("_new");
  fields.text = "client-order-42";

  const OrderFeedbackParseResult result = ParseOne(fields, &stats, &collector);

  EXPECT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_EQ(result.events_emitted, 0);
  EXPECT_EQ(collector.count, 0U);
  EXPECT_EQ(stats.invalid_text_count, 1U);
  EXPECT_EQ(stats.dropped_events, 1U);
}

TEST(GateOrderFeedbackParserTest, TruncatedChannelDoesNotEmitEvent) {
  std::array<char, 512> buffer{};
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  const std::string_view payload =
      BuildOrderFeedbackOrdersPayload(&buffer, MakeFields("_new"));

  const OrderFeedbackParseResult result = ParseGateOrderFeedbackMessage(
      payload.substr(0, payload.size() - 1), kLocalReceiveNs, stats, collector);

  EXPECT_EQ(result.status, OrderFeedbackParseStatus::kMalformedPayload);
  EXPECT_EQ(result.events_emitted, 0);
  EXPECT_EQ(collector.count, 0U);
  EXPECT_EQ(stats.malformed_payload_count, 1U);
  EXPECT_EQ(stats.events_emitted, 0U);
}

TEST(GateOrderFeedbackParserTest, UnexpectedChannelDoesNotEmitEvent) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderFeedbackPayloadFields fields = MakeFields("_new");

  const OrderFeedbackParseResult result =
      ParseOne(fields, &stats, &collector, "futures.usertrades");

  EXPECT_EQ(result.status, OrderFeedbackParseStatus::kUnexpectedChannel);
  EXPECT_EQ(result.events_emitted, 0);
  EXPECT_EQ(collector.count, 0U);
  EXPECT_EQ(stats.unexpected_channel_count, 1U);
  EXPECT_EQ(stats.events_emitted, 0U);
}

TEST(GateOrderFeedbackParserTest, TrailingBytesDoNotEmitEvent) {
  std::array<char, 512> buffer{};
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  std::string_view payload =
      BuildOrderFeedbackOrdersPayload(&buffer, MakeFields("_new"));
  buffer[payload.size()] = '\x01';
  payload = {buffer.data(), payload.size() + 1};

  const OrderFeedbackParseResult result =
      ParseGateOrderFeedbackMessage(payload, kLocalReceiveNs, stats, collector);

  EXPECT_EQ(result.status, OrderFeedbackParseStatus::kMalformedPayload);
  EXPECT_EQ(result.events_emitted, 0);
  EXPECT_EQ(collector.count, 0U);
  EXPECT_EQ(stats.malformed_payload_count, 1U);
  EXPECT_EQ(stats.events_emitted, 0U);
}

TEST(GateOrderFeedbackParserTest, ParsesMultipleOrdersAfterPayloadValidation) {
  std::array<char, 1024> buffer{};
  OrderFeedbackParserStats stats{};
  EventCollector<2> collector{};
  std::array<OrderFeedbackPayloadFields, 2> fields{MakeFields("_new"),
                                                   MakeFields("filled")};
  fields[1].left_mantissa = 0;
  fields[1].role = "maker";
  const std::string_view payload =
      BuildOrderFeedbackOrdersPayload(&buffer, fields);

  const OrderFeedbackParseResult result =
      ParseGateOrderFeedbackMessage(payload, kLocalReceiveNs, stats, collector);

  ASSERT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_EQ(result.orders_seen, 2);
  EXPECT_EQ(result.events_emitted, 2);
  ASSERT_EQ(collector.count, 2U);
  EXPECT_EQ(collector.events[0].kind, OrderFeedbackKind::kAccepted);
  EXPECT_EQ(collector.events[1].kind, OrderFeedbackKind::kFilled);
  EXPECT_EQ(collector.events[1].role, OrderRole::kMaker);
  EXPECT_EQ(stats.orders_seen, 2U);
  EXPECT_EQ(stats.events_emitted, 2U);
}

TEST(GateOrderFeedbackParserTest, MultipleOrdersUnexpectedChannelDoesNotEmit) {
  std::array<char, 1024> buffer{};
  OrderFeedbackParserStats stats{};
  EventCollector<2> collector{};
  std::array<OrderFeedbackPayloadFields, 2> fields{MakeFields("_new"),
                                                   MakeFields("filled")};
  fields[1].left_mantissa = 0;
  const std::string_view payload =
      BuildOrderFeedbackOrdersPayload(&buffer, fields, "futures.usertrades");

  const OrderFeedbackParseResult result =
      ParseGateOrderFeedbackMessage(payload, kLocalReceiveNs, stats, collector);

  EXPECT_EQ(result.status, OrderFeedbackParseStatus::kUnexpectedChannel);
  EXPECT_EQ(result.orders_seen, 0);
  EXPECT_EQ(result.events_emitted, 0);
  EXPECT_EQ(collector.count, 0U);
  EXPECT_EQ(stats.unexpected_channel_count, 1U);
  EXPECT_EQ(stats.orders_seen, 0U);
  EXPECT_EQ(stats.events_emitted, 0U);
}

TEST(GateOrderFeedbackParserTest, MapsIntegralDecimalSizeExponent) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderFeedbackPayloadFields fields = MakeFields("filled");
  fields.size_exponent = -3;
  fields.size_mantissa = 10'000;
  fields.left_mantissa = 0;

  const OrderFeedbackParseResult result = ParseOne(fields, &stats, &collector);

  EXPECT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  ASSERT_EQ(collector.count, 1U);
  EXPECT_EQ(collector.events[0].kind, OrderFeedbackKind::kFilled);
  EXPECT_EQ(collector.events[0].cumulative_filled_quantity, 10);
  EXPECT_EQ(collector.events[0].left_quantity, 0);
  EXPECT_EQ(stats.events_emitted, 1U);
  EXPECT_EQ(stats.dropped_events, 0U);
}

TEST(GateOrderFeedbackParserTest, DropsNonIntegralDecimalQuantity) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderFeedbackPayloadFields fields = MakeFields("_update");
  fields.size_exponent = -1;
  fields.size_mantissa = 15;
  fields.left_mantissa = 5;

  const OrderFeedbackParseResult result = ParseOne(fields, &stats, &collector);

  EXPECT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_EQ(collector.count, 0U);
  EXPECT_EQ(stats.invalid_quantity_count, 1U);
  EXPECT_EQ(stats.dropped_events, 1U);
}

TEST(GateOrderFeedbackParserTest, DropsFilledWithNonZeroLeft) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderFeedbackPayloadFields fields = MakeFields("filled");
  fields.left_mantissa = 1;

  const OrderFeedbackParseResult result = ParseOne(fields, &stats, &collector);

  EXPECT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_EQ(collector.count, 0U);
  EXPECT_EQ(stats.unsupported_filled_left_count, 1U);
  EXPECT_EQ(stats.dropped_events, 1U);
}

TEST(GateOrderFeedbackParserTest, ConvertsExchangeUpdateTimeToNanoseconds) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderFeedbackPayloadFields fields = MakeFields("_new");
  fields.update_time_us = 123'456'789;

  const OrderFeedbackParseResult result = ParseOne(fields, &stats, &collector);

  ASSERT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  ASSERT_EQ(collector.count, 1U);
  EXPECT_EQ(collector.events[0].exchange_update_ns, 123'456'789'000);
}

TEST(GateOrderFeedbackParserTest, MapsRoleMakerTakerAndNone) {
  struct Case {
    std::string_view role;
    OrderRole expected;
  };
  static constexpr std::array<Case, 3> kCases{{
      {"maker", OrderRole::kMaker},
      {"taker", OrderRole::kTaker},
      {"", OrderRole::kNone},
  }};

  for (const Case& test_case : kCases) {
    OrderFeedbackParserStats stats{};
    EventCollector collector{};
    OrderFeedbackPayloadFields fields = MakeFields("filled");
    fields.left_mantissa = 0;
    fields.role = test_case.role;

    const OrderFeedbackParseResult result =
        ParseOne(fields, &stats, &collector);

    ASSERT_EQ(result.status, OrderFeedbackParseStatus::kOk);
    ASSERT_EQ(collector.count, 1U) << test_case.role;
    EXPECT_EQ(collector.events[0].role, test_case.expected) << test_case.role;
  }
}

}  // namespace aquila::gate
