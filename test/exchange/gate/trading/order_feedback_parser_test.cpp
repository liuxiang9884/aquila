#include "exchange/gate/trading/order_feedback_parser.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>
#include <sbepp/sbepp.hpp>

#include "core/trading/order_feedback_event.h"
#include "core/trading/order_id.h"
#include "exchange/gate/sbe/generated/gate/messages/orders.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"
#include "exchange/gate/sbe/message_dispatcher.h"

namespace aquila::gate {
namespace {

constexpr std::uint64_t kLocalOrderId = LocalOrderIdCodec::Encode(3, 42);
constexpr std::uint64_t kExchangeOrderId = 36'028'827'892'199'865ULL;
constexpr std::int64_t kLocalReceiveNs = 1'770'000'000'001'222'000;
constexpr std::int64_t kUpdateTimeUs = 1'770'000'000'001'111;
constexpr std::int64_t kUpdateTimeNs = kUpdateTimeUs * 1000;

struct OrderPayloadFields {
  std::uint64_t local_order_id{kLocalOrderId};
  std::uint64_t exchange_order_id{kExchangeOrderId};
  std::int8_t size_exponent{0};
  std::int64_t left_mantissa{4};
  std::int64_t size_mantissa{10};
  std::int64_t update_time_us{kUpdateTimeUs};
  std::int8_t price_exponent{-2};
  std::int64_t fill_price_mantissa{6'501'250};
  std::string_view role{};
  std::string_view text{"t-216172782113783850"};
  std::string_view finish_as{"_update"};
};

template <typename T, std::size_t N>
void WriteLittleEndian(std::array<char, N>& buffer, std::size_t offset,
                       T value) noexcept {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

template <std::size_t N>
void AppendVarString(std::array<char, N>& buffer, std::size_t* offset,
                     std::string_view value) noexcept {
  buffer[(*offset)++] = static_cast<char>(value.size());
  std::memcpy(buffer.data() + *offset, value.data(), value.size());
  *offset += value.size();
}

template <std::size_t N>
std::string_view BuildOrdersPayload(std::array<char, N>* buffer,
                                    const OrderPayloadFields& fields) noexcept {
  static constexpr std::uint16_t kMessageBlockLength =
      ::sbepp::message_traits<::gate::schema::messages::orders>::block_length();
  static constexpr std::uint16_t kResultBlockLength = ::sbepp::group_traits<
      ::gate::schema::messages::orders::result>::block_length();
  static_assert(kMessageBlockLength == 9);
  static_assert(kResultBlockLength == 156);

  std::size_t offset = 0;
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kMessageBlockLength);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kGateSbeOrdersTemplateId);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kGateSbeSchemaId);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kGateSbeSchemaVersion);
  offset += sizeof(std::uint16_t);

  WriteLittleEndian<std::int64_t>(*buffer, offset, kUpdateTimeUs);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(
      *buffer, offset, static_cast<std::int8_t>(::gate::types::Event::Update));
  offset += sizeof(std::int8_t);

  WriteLittleEndian<std::uint16_t>(*buffer, offset, kResultBlockLength);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, 1);
  offset += sizeof(std::uint16_t);

  const std::size_t entry_offset = offset;
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 0,
                                  kUpdateTimeUs - 10);
  WriteLittleEndian<std::uint64_t>(*buffer, entry_offset + 8,
                                   fields.exchange_order_id);
  WriteLittleEndian<std::int8_t>(*buffer, entry_offset + 16,
                                 fields.size_exponent);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 25,
                                  fields.left_mantissa);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 33,
                                  fields.size_mantissa);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 57, 0);
  WriteLittleEndian<std::int8_t>(*buffer, entry_offset + 65, 1);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 66,
                                  fields.update_time_us);
  WriteLittleEndian<std::int8_t>(*buffer, entry_offset + 87,
                                 fields.price_exponent);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 88,
                                  fields.fill_price_mantissa);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 96,
                                  fields.fill_price_mantissa);
  offset += kResultBlockLength;

  AppendVarString(*buffer, &offset, "BTC_USDT");
  AppendVarString(*buffer, &offset, fields.role);
  AppendVarString(*buffer, &offset, fields.text);
  AppendVarString(*buffer, &offset, "gtc");
  AppendVarString(*buffer, &offset, fields.finish_as);
  AppendVarString(*buffer, &offset, "open");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "100");
  AppendVarString(*buffer, &offset, "futures.orders");
  return {buffer->data(), offset};
}

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

OrderPayloadFields MakeFields(std::string_view finish_as) noexcept {
  OrderPayloadFields fields{};
  fields.finish_as = finish_as;
  fields.text = "t-216172782113783850";
  return fields;
}

template <std::size_t N = 1>
OrderFeedbackParseResult ParseOne(const OrderPayloadFields& fields,
                                  OrderFeedbackParserStats* stats,
                                  EventCollector<N>* collector) {
  std::array<char, 512> buffer{};
  const std::string_view payload = BuildOrdersPayload(&buffer, fields);
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
  OrderPayloadFields fields = MakeFields("_update");
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
  OrderPayloadFields fields = MakeFields("filled");
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
  OrderPayloadFields fields = MakeFields("cancelled");
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
  OrderPayloadFields fields = MakeFields("_new");
  fields.text = "client-order-42";

  const OrderFeedbackParseResult result = ParseOne(fields, &stats, &collector);

  EXPECT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_EQ(result.events_emitted, 0);
  EXPECT_EQ(collector.count, 0U);
  EXPECT_EQ(stats.invalid_text_count, 1U);
  EXPECT_EQ(stats.dropped_events, 1U);
}

TEST(GateOrderFeedbackParserTest, DropsUnsupportedSizeExponent) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderPayloadFields fields = MakeFields("_update");
  fields.size_exponent = -1;

  const OrderFeedbackParseResult result = ParseOne(fields, &stats, &collector);

  EXPECT_EQ(result.status, OrderFeedbackParseStatus::kOk);
  EXPECT_EQ(collector.count, 0U);
  EXPECT_EQ(stats.unsupported_size_exponent_count, 1U);
  EXPECT_EQ(stats.dropped_events, 1U);
}

TEST(GateOrderFeedbackParserTest, DropsFilledWithNonZeroLeft) {
  OrderFeedbackParserStats stats{};
  EventCollector collector{};
  OrderPayloadFields fields = MakeFields("filled");
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
  OrderPayloadFields fields = MakeFields("_new");
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
    OrderPayloadFields fields = MakeFields("filled");
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
