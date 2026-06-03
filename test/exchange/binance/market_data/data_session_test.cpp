#include "exchange/binance/market_data/data_session.h"

#include <array>
#include <concepts>
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

struct RecordingDataSink {
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

using DefaultNoStatsSession = aquila::binance::DataSession<RecordingDataSink>;
using Session =
    aquila::binance::DataSession<RecordingDataSink,
                                 aquila::binance::DefaultTlsWebSocketPolicy,
                                 aquila::binance::SessionOnlyDiagnosticsPolicy>;
using DiagnosticSession =
    aquila::binance::DataSession<RecordingDataSink,
                                 aquila::binance::DefaultTlsWebSocketPolicy,
                                 aquila::binance::DataSessionDiagnosticsPolicy>;
using CoarseClockSession =
    aquila::binance::DataSession<RecordingDataSink, CoarseClockWebSocketPolicy>;

static_assert(!DefaultNoStatsSession::SessionDiagnosticsEnabled);
static_assert(Session::SessionDiagnosticsEnabled);
static_assert(Session::kClockSource ==
              aquila::websocket::ClockSource::kRealtime);
static_assert(CoarseClockSession::kClockSource ==
              aquila::websocket::ClockSource::kMonotonicCoarse);
static_assert(!std::is_constructible_v<
              Session, aquila::websocket::ConnectionConfig,
              std::span<const aquila::binance::SymbolBinding>,
              RecordingDataSink&, aquila::websocket::ClockSource>);
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

Session MakeSession(RecordingDataSink& data_sink) {
  static constexpr std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "fstream.binance.com";
  config.port = "443";
  return Session(std::move(config), symbols, data_sink);
}

DefaultNoStatsSession MakeDefaultNoStatsSession(RecordingDataSink& data_sink) {
  static constexpr std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "fstream.binance.com";
  config.port = "443";
  return DefaultNoStatsSession(std::move(config), symbols, data_sink);
}

DiagnosticSession MakeDiagnosticSession(RecordingDataSink& data_sink) {
  static constexpr std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "fstream.binance.com";
  config.port = "443";
  return DiagnosticSession(std::move(config), symbols, data_sink);
}

}  // namespace

TEST(BinanceDataSessionTest, BuildsRawStreamTargetFromSymbols) {
  RecordingDataSink data_sink;
  Session session = MakeSession(data_sink);

  EXPECT_EQ(session.stream_target(), "/public/ws/btcusdt@bookTicker");
}

TEST(BinanceDataSessionTest, DelegatesTextBookTickerToClient) {
  RecordingDataSink data_sink;
  Session session = MakeSession(data_sink);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().text_messages, 1U);
  EXPECT_EQ(session.stats().book_ticker_messages, 1U);
  ASSERT_EQ(data_sink.calls, 1);
  EXPECT_EQ(data_sink.last.symbol_id, 11);
  EXPECT_EQ(data_sink.last.id, 400900217);
}

TEST(BinanceDataSessionTest, IgnoresBinaryPayload) {
  RecordingDataSink data_sink;
  Session session = MakeSession(data_sink);

  const auto result = session.Handle(BinaryView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().binary_messages, 1U);
  EXPECT_EQ(data_sink.calls, 0);
}

TEST(BinanceDataSessionTest, DefaultSessionDiagnosticsDoNotCount) {
  RecordingDataSink data_sink;
  DefaultNoStatsSession session = MakeDefaultNoStatsSession(data_sink);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(data_sink.calls, 1);
  EXPECT_EQ(session.stats().text_messages, 0U);
  EXPECT_EQ(session.stats().book_ticker_messages, 0U);
}

TEST(BinanceDataSessionTest, ExposesMarketDataDiagnosticsWhenEnabled) {
  RecordingDataSink data_sink;
  DiagnosticSession session = MakeDiagnosticSession(data_sink);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(data_sink.calls, 1);
  EXPECT_EQ(
      session.market_data_client_diagnostics().stats().book_ticker_messages,
      1U);
}

TEST(BinanceDataSessionTest, RejectsEmptyStreamTargetOnStart) {
  static constexpr std::array<aquila::binance::SymbolBinding, 0> symbols{};
  RecordingDataSink data_sink;
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.port = "1";
  Session session(std::move(config), symbols, data_sink);

  EXPECT_EQ(session.stream_target(), "");
  EXPECT_FALSE(session.Start());
  EXPECT_EQ(session.phase(), aquila::websocket::ConnectionPhase::kDisconnected);
  EXPECT_EQ(session.last_error(), aquila::websocket::ConnectionError::kNone);
}
