#include "exchange/binance/market_data/data_session.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "core/websocket/message_view.h"

namespace {

constexpr std::string_view kBookTickerJson =
    R"({"e":"bookTicker","u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";

aquila::websocket::MessageView TextView(std::string_view payload) noexcept {
  return {
      .kind = aquila::websocket::PayloadKind::kText,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 8,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

aquila::websocket::MessageView BinaryView(std::string_view payload) noexcept {
  return {
      .kind = aquila::websocket::PayloadKind::kBinary,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 9,
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

struct CoarseClockWebSocketPolicy : aquila::binance::DefaultTlsWebSocketPolicy {
  static constexpr aquila::websocket::ClockSource kClockSource =
      aquila::websocket::ClockSource::kMonotonicCoarse;
};

using DefaultNoStatsSession = aquila::binance::DataSession<RecordingConsumer>;
using Session =
    aquila::binance::DataSession<RecordingConsumer,
                                 aquila::binance::DefaultTlsWebSocketPolicy,
                                 aquila::binance::SessionOnlyDiagnosticsPolicy>;
using DiagnosticSession =
    aquila::binance::DataSession<RecordingConsumer,
                                 aquila::binance::DefaultTlsWebSocketPolicy,
                                 aquila::binance::DataSessionDiagnosticsPolicy>;
using CoarseClockSession =
    aquila::binance::DataSession<RecordingConsumer, CoarseClockWebSocketPolicy>;

static_assert(!DefaultNoStatsSession::SessionDiagnosticsEnabled);
static_assert(Session::SessionDiagnosticsEnabled);
static_assert(Session::kClockSource ==
              aquila::websocket::DefaultWebSocketOptions::kClockSource);
static_assert(CoarseClockSession::kClockSource ==
              aquila::websocket::ClockSource::kMonotonicCoarse);
static_assert(!std::is_constructible_v<
              Session, aquila::websocket::ConnectionConfig,
              std::span<const aquila::binance::SymbolBinding>,
              RecordingConsumer&, aquila::websocket::ClockSource>);

Session MakeSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "fstream.binance.com";
  config.service = "443";
  return Session(std::move(config), symbols, consumer);
}

DefaultNoStatsSession MakeDefaultNoStatsSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "fstream.binance.com";
  config.service = "443";
  return DefaultNoStatsSession(std::move(config), symbols, consumer);
}

DiagnosticSession MakeDiagnosticSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "fstream.binance.com";
  config.service = "443";
  return DiagnosticSession(std::move(config), symbols, consumer);
}

}  // namespace

TEST(BinanceDataSessionTest, BuildsRawStreamTargetFromSymbols) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  EXPECT_EQ(session.stream_target(), "/public/ws/btcusdt@bookTicker");
}

TEST(BinanceDataSessionTest, DelegatesTextBookTickerToClient) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().text_messages, 1U);
  EXPECT_EQ(session.stats().book_ticker_messages, 1U);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.id, 400900217);
}

TEST(BinanceDataSessionTest, IgnoresBinaryPayload) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(BinaryView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().binary_messages, 1U);
  EXPECT_EQ(consumer.calls, 0);
}

TEST(BinanceDataSessionTest, DefaultSessionDiagnosticsDoNotCount) {
  RecordingConsumer consumer;
  DefaultNoStatsSession session = MakeDefaultNoStatsSession(consumer);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 1);
  EXPECT_EQ(session.stats().text_messages, 0U);
  EXPECT_EQ(session.stats().book_ticker_messages, 0U);
}

TEST(BinanceDataSessionTest, ExposesMarketDataDiagnosticsWhenEnabled) {
  RecordingConsumer consumer;
  DiagnosticSession session = MakeDiagnosticSession(consumer);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 1);
  EXPECT_EQ(
      session.market_data_client_diagnostics().stats().book_ticker_messages,
      1U);
}

TEST(BinanceDataSessionTest, RejectsEmptyStreamTargetOnStart) {
  static constexpr std::array<aquila::binance::SymbolBinding, 0> symbols{};
  RecordingConsumer consumer;
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "1";
  Session session(std::move(config), symbols, consumer);

  EXPECT_EQ(session.stream_target(), "");
  EXPECT_FALSE(session.Start());
  EXPECT_EQ(session.phase(), aquila::websocket::ConnectionPhase::kDisconnected);
  EXPECT_EQ(session.last_error(), aquila::websocket::ConnectionError::kNone);
}
