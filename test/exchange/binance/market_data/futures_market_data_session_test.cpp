#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "core/websocket/message_view.h"
#include "exchange/binance/market_data/book_ticker_yyjson_parser.h"
#include "exchange/binance/market_data/session.h"

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

struct CoarseClockOptions : aquila::websocket::DefaultWebSocketOptions {
  static constexpr aquila::websocket::ClockSource kClockSource =
      aquila::websocket::ClockSource::kMonotonicCoarse;
};

using DefaultNoStatsSession =
    aquila::binance::FuturesMarketDataSession<RecordingConsumer>;
using Session = aquila::binance::FuturesMarketDataSession<
    RecordingConsumer, aquila::websocket::TlsSocket,
    aquila::binance::NoopFuturesMarketDataDiagnostics,
    aquila::websocket::DefaultWebSocketOptions,
    aquila::binance::FuturesMarketDataSessionDiagnostics>;
using DiagnosticSession = aquila::binance::FuturesMarketDataSession<
    RecordingConsumer, aquila::websocket::TlsSocket,
    aquila::binance::FuturesMarketDataDiagnostics,
    aquila::websocket::DefaultWebSocketOptions,
    aquila::binance::FuturesMarketDataSessionDiagnostics>;
using YyjsonSession = aquila::binance::FuturesMarketDataSession<
    RecordingConsumer, aquila::websocket::TlsSocket,
    aquila::binance::NoopFuturesMarketDataDiagnostics,
    aquila::websocket::DefaultWebSocketOptions,
    aquila::binance::FuturesMarketDataSessionDiagnostics,
    aquila::binance::YyjsonBookTickerParser>;
using CoarseClockSession = aquila::binance::FuturesMarketDataSession<
    RecordingConsumer, aquila::websocket::TlsSocket,
    aquila::binance::NoopFuturesMarketDataDiagnostics, CoarseClockOptions>;

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
      aquila::binance::SymbolBinding{.symbol = "ETHUSDT", .symbol_id = 12}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "fstream.binance.com";
  config.service = "443";
  return DiagnosticSession(std::move(config), symbols, consumer);
}

YyjsonSession MakeYyjsonSession(RecordingConsumer& consumer) {
  static constexpr std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "fstream.binance.com";
  config.service = "443";
  return YyjsonSession(std::move(config), symbols, consumer);
}

}  // namespace

TEST(BinanceFuturesMarketDataSessionTest, BuildsRawStreamTargetFromSymbols) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  EXPECT_EQ(session.stream_target(), "/public/ws/btcusdt@bookTicker");
}

TEST(BinanceFuturesMarketDataSessionTest, DelegatesTextBookTickerToClient) {
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

TEST(BinanceFuturesMarketDataSessionTest,
     DelegatesTextBookTickerToYyjsonClient) {
  RecordingConsumer consumer;
  YyjsonSession session = MakeYyjsonSession(consumer);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().text_messages, 1U);
  EXPECT_EQ(session.stats().book_ticker_messages, 1U);
  ASSERT_EQ(consumer.calls, 1);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.id, 400900217);
}

TEST(BinanceFuturesMarketDataSessionTest, IgnoresBinaryPayload) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);

  const auto result = session.Handle(BinaryView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().binary_messages, 1U);
  EXPECT_EQ(consumer.calls, 0);
}

TEST(BinanceFuturesMarketDataSessionTest, CountsNonFinalFrames) {
  RecordingConsumer consumer;
  Session session = MakeSession(consumer);
  aquila::websocket::MessageView view = TextView(kBookTickerJson);
  view.fin = false;

  const auto result = session.Handle(view);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().non_final_messages, 1U);
  EXPECT_EQ(consumer.calls, 0);
}

TEST(BinanceFuturesMarketDataSessionTest, DefaultSessionDiagnosticsDoNotCount) {
  RecordingConsumer consumer;
  DefaultNoStatsSession session = MakeDefaultNoStatsSession(consumer);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 1);
  EXPECT_EQ(session.stats().text_messages, 0U);
  EXPECT_EQ(session.stats().book_ticker_messages, 0U);
}

TEST(BinanceFuturesMarketDataSessionTest,
     ExposesMarketDataDiagnosticsWhenEnabled) {
  RecordingConsumer consumer;
  DiagnosticSession session = MakeDiagnosticSession(consumer);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.calls, 0);
  EXPECT_EQ(session.market_data_client_diagnostics().stats().unknown_symbols,
            1U);
}
