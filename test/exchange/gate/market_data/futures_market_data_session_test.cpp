#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "core/websocket/message_view.h"
#include "exchange/gate/market_data/session.h"
#include "test/exchange/gate/sbe/book_ticker_payload_builder.h"
#include <simdjson.h>

namespace {

using aquila::gate::test_support::BuildBookTickerPayload;

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

struct CoarseClockOptions : aquila::websocket::DefaultWebSocketOptions {
  static constexpr aquila::websocket::ClockSource kClockSource =
      aquila::websocket::ClockSource::kMonotonicCoarse;
};

using DefaultNoStatsSession =
    aquila::gate::FuturesMarketDataSession<RecordingConsumer>;
using Session = aquila::gate::FuturesMarketDataSession<
    RecordingConsumer, aquila::websocket::TlsSocket,
    aquila::gate::NoopFuturesMarketDataDiagnostics,
    aquila::websocket::DefaultWebSocketOptions,
    aquila::gate::FuturesMarketDataSessionDiagnostics>;
using DiagnosticSession = aquila::gate::FuturesMarketDataSession<
    RecordingConsumer, aquila::websocket::TlsSocket,
    aquila::gate::FuturesMarketDataDiagnostics,
    aquila::websocket::DefaultWebSocketOptions,
    aquila::gate::FuturesMarketDataSessionDiagnostics>;
using CoarseClockSession = aquila::gate::FuturesMarketDataSession<
    RecordingConsumer, aquila::websocket::TlsSocket,
    aquila::gate::NoopFuturesMarketDataDiagnostics, CoarseClockOptions>;

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

struct StateCapture {
  int state_calls{0};
  int error_calls{0};
  aquila::websocket::ConnectionPhase last_phase{
      aquila::websocket::ConnectionPhase::kDisconnected};
  aquila::websocket::ConnectionError last_error{
      aquila::websocket::ConnectionError::kNone};
};

void CaptureState(void* context,
                  aquila::websocket::ConnectionPhase phase) noexcept {
  auto* capture = static_cast<StateCapture*>(context);
  ++capture->state_calls;
  capture->last_phase = phase;
}

void CaptureError(void* context,
                  aquila::websocket::ConnectionError error) noexcept {
  auto* capture = static_cast<StateCapture*>(context);
  ++capture->error_calls;
  capture->last_error = error;
}

Session MakeSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "443";
  config.target = "/v4/ws/usdt/sbe?sbe_schema_id=1";
  return Session(std::move(config), symbols, consumer);
}

DiagnosticSession MakeDiagnosticSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "443";
  config.target = "/v4/ws/usdt/sbe?sbe_schema_id=1";
  return DiagnosticSession(std::move(config), symbols, consumer);
}

Session MakeSessionWithNoPreparedWriteSlots(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "443";
  config.target = "/v4/ws/usdt/sbe?sbe_schema_id=1";
  config.prepared_write_slots = 0;
  return Session(std::move(config), symbols, consumer);
}

DefaultNoStatsSession MakeDefaultNoStatsSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.symbol = "BTC_USDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "443";
  config.target = "/v4/ws/usdt/sbe?sbe_schema_id=1";
  return DefaultNoStatsSession(std::move(config), symbols, consumer);
}

}  // namespace

TEST(GateFuturesMarketDataSessionTest, MarksSubscribeAckSubscribed) {
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

TEST(GateFuturesMarketDataSessionTest, ParsesPaddedSubscribeAckWithoutCopy) {
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

TEST(GateFuturesMarketDataSessionTest, SendsSubscribeWhenActive) {
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

TEST(GateFuturesMarketDataSessionTest, RetriesSubscribeAfterActiveFailure) {
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

TEST(GateFuturesMarketDataSessionTest,
     ManualConnectionPhaseOnlyDrivesGateSubscriptionState) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);
  StateCapture capture;
  session.SetStateHandler(&capture, &CaptureState);
  session.SetErrorHandler(&capture, &CaptureError);

  EXPECT_EQ(session.phase(), aquila::websocket::ConnectionPhase::kDisconnected);
  EXPECT_EQ(session.last_error(), aquila::websocket::ConnectionError::kNone);

  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kActive);

  EXPECT_EQ(capture.state_calls, 0);
  EXPECT_EQ(capture.error_calls, 0);
  EXPECT_EQ(session.phase(), aquila::websocket::ConnectionPhase::kDisconnected);
  EXPECT_EQ(session.last_error(), aquila::websocket::ConnectionError::kNone);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kSubscribeSent);
}

TEST(GateFuturesMarketDataSessionTest, MarksUnsubscribeAckUnsubscribed) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(
      R"({"time":1,"channel":"futures.book_ticker","event":"unsubscribe","result":{"status":"success"}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kUnsubscribed);
  EXPECT_EQ(session.stats().unsubscribe_acks, 1U);
}

TEST(GateFuturesMarketDataSessionTest, RecordsControlErrorAsRejected) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(
      R"({"time":1,"channel":"futures.book_ticker","event":"subscribe","error":{"label":"INVALID_PARAM","message":"bad"}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.subscription_state(),
            aquila::gate::SubscriptionState::kRejected);
  EXPECT_EQ(session.stats().control_errors, 1U);
}

TEST(GateFuturesMarketDataSessionTest, MalformedTextIsAcceptedAndCounted) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView("{"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().text_messages, 1U);
  EXPECT_EQ(session.stats().control_parse_errors, 1U);
}

TEST(GateFuturesMarketDataSessionTest, JsonUpdateRoutesToUnsupportedCounter) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(
      R"({"time":1,"channel":"futures.some_json_only_channel","event":"update","result":{"x":1}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().json_market_data_messages, 1U);
  EXPECT_EQ(session.stats().unsupported_json_market_data_messages, 1U);
  EXPECT_EQ(consumer.calls, 0);
}

TEST(GateFuturesMarketDataSessionTest, DelegatesBinaryBookTickerToClient) {
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

TEST(GateFuturesMarketDataSessionTest,
     ExposesMarketDataDiagnosticsWhenEnabled) {
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

TEST(GateFuturesMarketDataSessionTest, DefaultSessionDiagnosticsDoNotCount) {
  RecordingConsumer consumer;
  DefaultNoStatsSession session = MakeDefaultNoStatsSession(consumer);
  std::array<char, 192> buffer{};
  const std::string_view payload = BuildBookTickerPayload(&buffer, "BTC_USDT");

  const auto result = session.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 1);
  EXPECT_EQ(session.stats().binary_messages, 0U);
}
