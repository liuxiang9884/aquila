#include "exchange/gate/market_data/data_session.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "core/websocket/message_view.h"
#include "evaluation/exchange/gate/sbe/book_ticker_payload_builder.h"
#include <simdjson.h>

namespace {

using aquila::gate::evaluation::BuildBookTickerPayload;

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

struct CoarseClockWebSocketPolicy : aquila::gate::DefaultTlsWebSocketPolicy {
  static constexpr aquila::websocket::ClockSource kClockSource =
      aquila::websocket::ClockSource::kMonotonicCoarse;
};

using DefaultNoStatsSession = aquila::gate::DataSession<RecordingConsumer>;
using Session =
    aquila::gate::DataSession<RecordingConsumer,
                              aquila::gate::DefaultTlsWebSocketPolicy,
                              aquila::gate::SessionOnlyDiagnosticsPolicy>;
using DiagnosticSession =
    aquila::gate::DataSession<RecordingConsumer,
                              aquila::gate::DefaultTlsWebSocketPolicy,
                              aquila::gate::DataSessionDiagnosticsPolicy>;
using CoarseClockSession =
    aquila::gate::DataSession<RecordingConsumer, CoarseClockWebSocketPolicy>;

static_assert(!DefaultNoStatsSession::SessionDiagnosticsEnabled);
static_assert(Session::SessionDiagnosticsEnabled);
static_assert(Session::kClockSource ==
              aquila::websocket::DefaultWebSocketOptions::kClockSource);
static_assert(CoarseClockSession::kClockSource ==
              aquila::websocket::ClockSource::kMonotonicCoarse);
static_assert(!std::is_constructible_v<
              Session, aquila::websocket::ConnectionConfig,
              std::span<const aquila::gate::SymbolBinding>, RecordingConsumer&,
              aquila::websocket::ClockSource>);
template <typename SessionT>
concept HasStateHandler =
    requires(SessionT& session) { session.SetStateHandler(nullptr, nullptr); };

template <typename SessionT>
concept HasErrorHandler =
    requires(SessionT& session) { session.SetErrorHandler(nullptr, nullptr); };

template <typename SessionT>
concept HasRun = requires(SessionT& session) {
  { session.Run() } -> std::same_as<bool>;
};

static_assert(!HasStateHandler<Session>);
static_assert(!HasErrorHandler<Session>);
static_assert(HasRun<Session>);

Session MakeSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "443";
  config.target = "/v4/ws/usdt/sbe?sbe_schema_id=1";
  return Session(std::move(config), symbols, consumer);
}

DiagnosticSession MakeDiagnosticSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "443";
  config.target = "/v4/ws/usdt/sbe?sbe_schema_id=1";
  return DiagnosticSession(std::move(config), symbols, consumer);
}

Session MakeSessionWithNoPreparedWriteSlots(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "443";
  config.target = "/v4/ws/usdt/sbe?sbe_schema_id=1";
  config.prepared_write_slots = 0;
  return Session(std::move(config), symbols, consumer);
}

DefaultNoStatsSession MakeDefaultNoStatsSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "443";
  config.target = "/v4/ws/usdt/sbe?sbe_schema_id=1";
  return DefaultNoStatsSession(std::move(config), symbols, consumer);
}

}  // namespace

TEST(GateDataSessionTest, MarksSubscribeAckSubscribed) {
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

TEST(GateDataSessionTest, ParsesPaddedSubscribeAckWithoutCopy) {
  static constexpr std::string_view kSubscribeAck =
      R"({"time":1,"channel":"futures.book_ticker","event":"subscribe","result":{"status":"success"}})";
  std::array<char, kSubscribeAck.size() + simdjson::SIMDJSON_PADDING> buffer{};
  std::memcpy(buffer.data(), kSubscribeAck.data(), kSubscribeAck.size());
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);
  const aquila::websocket::MessageView view{
      .kind = aquila::websocket::PayloadKind::kText,
      .payload = std::as_bytes(std::span(buffer.data(), kSubscribeAck.size())),
      .sequence = 9,
      .fin = true,
      .readable_tail_bytes = simdjson::SIMDJSON_PADDING,
  };

  const auto result = session.Handle(view);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribed);
  EXPECT_EQ(session.stats().subscribe_acks, 1U);
}

TEST(GateDataSessionTest, SendsSubscribeWhenActive) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kActive);

  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribeSent);
  EXPECT_EQ(session.subscribe_status(), aquila::websocket::SendStatus::kOk);
  EXPECT_EQ(session.stats().subscribe_sent, 1U);
  EXPECT_NE(session.last_subscribe_request().find("futures.book_ticker"),
            std::string_view::npos);
  EXPECT_NE(session.last_subscribe_request().find("BTC_USDT"),
            std::string_view::npos);
}

TEST(GateDataSessionTest, RetriesSubscribeAfterActiveFailure) {
  RecordingConsumer consumer;
  Session session = MakeSessionWithNoPreparedWriteSlots(consumer);

  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kActive);
  const auto retry_status = session.RetryPendingSubscribe();

  EXPECT_EQ(session.subscribe_status(),
            aquila::websocket::SendStatus::kNoPreparedWriteSlot);
  EXPECT_EQ(retry_status, aquila::websocket::SendStatus::kNoPreparedWriteSlot);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kIdle);
  EXPECT_EQ(session.stats().subscribe_sent, 0U);
  EXPECT_EQ(session.stats().subscribe_retry_attempts, 1U);
  EXPECT_EQ(session.stats().subscribe_send_failures, 2U);
}

TEST(GateDataSessionTest,
     ManualConnectionPhaseOnlyDrivesGateSubscriptionState) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  EXPECT_FALSE(session.ever_active());
  EXPECT_EQ(session.phase(), aquila::websocket::ConnectionPhase::kDisconnected);
  EXPECT_EQ(session.last_error(), aquila::websocket::ConnectionError::kNone);

  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kActive);

  EXPECT_TRUE(session.ever_active());
  EXPECT_EQ(session.phase(), aquila::websocket::ConnectionPhase::kDisconnected);
  EXPECT_EQ(session.last_error(), aquila::websocket::ConnectionError::kNone);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribeSent);
}

TEST(GateDataSessionTest, MarksUnsubscribeAckUnsubscribed) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(
      R"({"time":1,"channel":"futures.book_ticker","event":"unsubscribe","result":{"status":"success"}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kUnsubscribed);
  EXPECT_EQ(session.stats().unsubscribe_acks, 1U);
}

TEST(GateDataSessionTest, RecordsControlErrorAsRejected) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(
      R"({"time":1,"channel":"futures.book_ticker","event":"subscribe","error":{"label":"INVALID_PARAM","message":"bad"}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kRejected);
  EXPECT_EQ(session.stats().control_errors, 1U);
}

TEST(GateDataSessionTest, MalformedTextIsAcceptedAndCounted) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView("{"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().text_messages, 1U);
  EXPECT_EQ(session.stats().control_parse_errors, 1U);
}

TEST(GateDataSessionTest, JsonUpdateRoutesToUnsupportedCounter) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(
      R"({"time":1,"channel":"futures.some_json_only_channel","event":"update","result":{"x":1}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().json_market_data_messages, 1U);
  EXPECT_EQ(session.stats().unsupported_json_market_data_messages, 1U);
  EXPECT_EQ(consumer.calls, 0);
}

TEST(GateDataSessionTest, DelegatesBinaryBookTickerToClient) {
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

TEST(GateDataSessionTest, ExposesMarketDataDiagnosticsWhenEnabled) {
  RecordingConsumer consumer;
  DiagnosticSession session = MakeDiagnosticSession(consumer);
  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = session.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 1);
  EXPECT_EQ(session.market_data_client_diagnostics()
                .stats()
                .unsupported_sbe_templates,
            0U);
}

TEST(GateDataSessionTest, DefaultSessionDiagnosticsDoNotCount) {
  RecordingConsumer consumer;
  DefaultNoStatsSession session = MakeDefaultNoStatsSession(consumer);
  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = session.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 1);
  EXPECT_EQ(session.stats().binary_messages, 0U);
}
