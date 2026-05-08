#include "exchange/gate/trading/order_feedback_session.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <gtest/gtest.h>
#include <sbepp/sbepp.hpp>

#include "core/trading/order_feedback_event.h"
#include "core/trading/order_id.h"
#include "core/websocket/message_view.h"
#include "exchange/gate/sbe/generated/gate/messages/orders.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"
#include "exchange/gate/sbe/message_dispatcher.h"

namespace aquila::gate {
namespace {

constexpr std::uint64_t kLocalOrderId = LocalOrderIdCodec::Encode(3, 42);
constexpr std::uint64_t kExchangeOrderId = 36'028'827'892'199'865ULL;
constexpr std::int64_t kLocalReceiveNs = 1'770'000'000'001'222'000;
constexpr std::int64_t kUpdateTimeUs = 1'770'000'000'001'111;

websocket::MessageView TextView(std::string_view payload) noexcept {
  return {
      .kind = websocket::PayloadKind::kText,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 1,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

websocket::MessageView BinaryView(std::string_view payload) noexcept {
  return {
      .kind = websocket::PayloadKind::kBinary,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 2,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

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
  std::string_view finish_as{"_new"};
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

struct RecordingPublisher {
  bool publish_result{true};
  bool global_gap_result{true};
  int publish_calls{0};
  int global_gap_calls{0};
  OrderFeedbackEvent last_event{};
  OrderFeedbackGapReason last_gap_reason{OrderFeedbackGapReason::kUnknown};
  std::int64_t last_gap_receive_ns{0};

  bool Publish(const OrderFeedbackEvent& event) noexcept {
    ++publish_calls;
    last_event = event;
    return publish_result;
  }

  bool PublishGlobalGap(OrderFeedbackGapReason reason,
                        std::int64_t local_receive_ns) noexcept {
    ++global_gap_calls;
    last_gap_reason = reason;
    last_gap_receive_ns = local_receive_ns;
    return global_gap_result;
  }
};

using Session =
    OrderFeedbackSession<RecordingPublisher,
                         OrderFeedbackSessionDefaultPlainWebSocketPolicy,
                         OrderFeedbackSessionDiagnostics>;

websocket::ConnectionConfig MakeConfig() {
  websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "80";
  config.target = "/v4/ws/usdt";
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 4096;
  return config;
}

Session MakeSession(RecordingPublisher& publisher) {
  return Session(MakeConfig(),
                 LoginCredentials{.api_key = "key", .api_secret = "secret"},
                 publisher);
}

std::string_view LoginSuccessResponse() noexcept {
  return R"({"request_id":"72057594037927937","ack":false,"header":{"status":"200","channel":"futures.login","event":"api"},"data":{"result":{"uid":"14391412"}}})";
}

std::string_view SubscribeSuccessResponse() noexcept {
  return R"({"time":1,"channel":"futures.orders","event":"subscribe","result":{"status":"success"}})";
}

}  // namespace

TEST(OrderFeedbackSessionTest, ActiveConnectionSendsLogin) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  EXPECT_FALSE(session.login_ready());
  EXPECT_NE(session.last_login_request().find(R"("channel":"futures.login")"),
            std::string_view::npos);
  EXPECT_EQ(session.stats().login_sent, 1U);
}

TEST(OrderFeedbackSessionTest, LoginSuccessSendsOrdersSubscribe) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);

  ASSERT_EQ(session.Handle(TextView(LoginSuccessResponse())),
            websocket::DeliveryResult::kAccepted);

  EXPECT_TRUE(session.login_ready());
  EXPECT_EQ(session.stats().login_accepted, 1U);
  EXPECT_NE(
      session.last_subscribe_request().find(R"("channel":"futures.orders")"),
      std::string_view::npos);
  EXPECT_NE(session.last_subscribe_request().find(R"("event":"subscribe")"),
            std::string_view::npos);
  EXPECT_NE(session.last_subscribe_request().find(R"("14391412")"),
            std::string_view::npos);
  EXPECT_NE(session.last_subscribe_request().find(R"("!all")"),
            std::string_view::npos);
  EXPECT_NE(session.last_subscribe_request().find(R"("SIGN":")"),
            std::string_view::npos);
  EXPECT_EQ(session.stats().subscribe_sent, 1U);
}

TEST(OrderFeedbackSessionTest, SubscribeAckMarksReady) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  ASSERT_EQ(session.Handle(TextView(LoginSuccessResponse())),
            websocket::DeliveryResult::kAccepted);

  ASSERT_EQ(session.Handle(TextView(SubscribeSuccessResponse())),
            websocket::DeliveryResult::kAccepted);

  EXPECT_TRUE(session.ready());
  EXPECT_EQ(session.stats().subscribe_acks, 1U);
}

TEST(OrderFeedbackSessionTest, BinaryOrdersPayloadPublishesEvent) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);
  std::array<char, 512> buffer{};
  const std::string_view payload = BuildOrdersPayload(&buffer, {});

  ASSERT_EQ(session.Handle(BinaryView(payload)),
            websocket::DeliveryResult::kAccepted);

  EXPECT_EQ(publisher.publish_calls, 1);
  EXPECT_EQ(publisher.last_event.kind, OrderFeedbackKind::kAccepted);
  EXPECT_EQ(publisher.last_event.local_order_id, kLocalOrderId);
  EXPECT_EQ(publisher.last_event.exchange_order_id, kExchangeOrderId);
  EXPECT_EQ(session.stats().binary_messages, 1U);
  EXPECT_EQ(session.stats().events_published, 1U);
}

TEST(OrderFeedbackSessionTest, MalformedBinaryPayloadIncrementsDiagnostics) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);

  ASSERT_EQ(session.Handle(BinaryView("x")),
            websocket::DeliveryResult::kAccepted);

  EXPECT_EQ(publisher.publish_calls, 0);
  EXPECT_EQ(session.stats().binary_messages, 1U);
  EXPECT_EQ(session.stats().parse_errors, 1U);
}

TEST(OrderFeedbackSessionTest, DisconnectAfterActivePublishesGlobalGap) {
  RecordingPublisher publisher;
  Session session = MakeSession(publisher);

  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  session.OnConnectionPhase(websocket::ConnectionPhase::kDisconnected);

  EXPECT_EQ(publisher.global_gap_calls, 1);
  EXPECT_EQ(publisher.last_gap_reason,
            OrderFeedbackGapReason::kSessionDisconnected);
  EXPECT_EQ(session.stats().global_gaps_published, 1U);
}

TEST(OrderFeedbackSessionTest, PublishFailureIncrementsDiagnostics) {
  RecordingPublisher publisher;
  publisher.publish_result = false;
  Session session = MakeSession(publisher);
  std::array<char, 512> buffer{};
  const std::string_view payload = BuildOrdersPayload(&buffer, {});

  ASSERT_EQ(session.Handle(BinaryView(payload)),
            websocket::DeliveryResult::kAccepted);

  EXPECT_EQ(publisher.publish_calls, 1);
  EXPECT_EQ(session.stats().events_published, 0U);
  EXPECT_EQ(session.stats().publish_failures, 1U);
}

}  // namespace aquila::gate
