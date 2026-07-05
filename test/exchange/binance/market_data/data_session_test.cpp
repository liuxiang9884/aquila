#include "exchange/binance/market_data/data_session.h"

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/market_data/data_shm.h"
#include "core/websocket/message_view.h"

namespace {

constexpr std::string_view kBookTickerJson =
    R"({"e":"bookTicker","u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";
constexpr std::string_view kTradeJson =
    R"({"e":"trade","E":1783228448495,"T":1783228448495,"s":"BTCUSDT","t":7868321828,"p":"62738.70","q":"0.002","X":"MARKET","m":false,"st":1})";

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
  int book_ticker_calls{0};
  int trade_calls{0};
  aquila::BookTicker last_book_ticker{};
  aquila::Trade last_trade{};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++book_ticker_calls;
    last_book_ticker = book_ticker;
  }

  void OnTrade(const aquila::Trade& trade) noexcept {
    ++trade_calls;
    last_trade = trade;
  }
};

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

  std::string shm_name;
};

std::string UniqueShmName(std::string_view suffix) {
  return fmt::format("/aquila_binance_data_session_test_{}_{}", ::getpid(),
                     suffix);
}

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

Session MakeTradeSession(RecordingDataSink& data_sink) {
  static constexpr std::array<aquila::binance::SymbolBinding, 1> symbols{
      aquila::binance::SymbolBinding{.symbol = "BTCUSDT", .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "fstream.binance.com";
  config.port = "443";
  return Session(
      std::move(config), symbols,
      aquila::binance::DataSessionFeeds{.book_ticker = false, .trade = true},
      data_sink);
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

TEST(BinanceDataSessionTest, BuildsMixedRawStreamTargetFromConfig) {
  RecordingDataSink data_sink;
  aquila::binance::DataSessionConfig config{};
  config.name = "binance_mixed";
  config.connection.host = "fstream.binance.com";
  config.connection.port = "443";
  config.exchange_symbols = {"BTCUSDT", "ETHUSDT"};
  config.symbol_ids = {11, 12};
  config.feeds = {.book_ticker = true, .trade = true};

  Session session(std::move(config), data_sink);

  EXPECT_EQ(session.stream_target(),
            "/public/ws/btcusdt@bookTicker/btcusdt@trade/"
            "ethusdt@bookTicker/ethusdt@trade");
}

TEST(BinanceDataSessionTest, DelegatesTextBookTickerToClient) {
  RecordingDataSink data_sink;
  Session session = MakeSession(data_sink);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().text_messages, 1U);
  EXPECT_EQ(session.stats().book_ticker_messages, 1U);
  ASSERT_EQ(data_sink.book_ticker_calls, 1);
  EXPECT_EQ(data_sink.last_book_ticker.symbol_id, 11);
  EXPECT_EQ(data_sink.last_book_ticker.id, 400900217);
}

TEST(BinanceDataSessionTest, DelegatesTextTradeToClient) {
  RecordingDataSink data_sink;
  Session session = MakeTradeSession(data_sink);

  const auto result = session.Handle(TextView(kTradeJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().text_messages, 1U);
  EXPECT_EQ(session.stats().trade_messages, 1U);
  ASSERT_EQ(data_sink.trade_calls, 1);
  EXPECT_EQ(data_sink.last_trade.symbol_id, 11);
  EXPECT_EQ(data_sink.last_trade.id, 7868321828LL);
  EXPECT_EQ(data_sink.last_trade.side, aquila::OrderSide::kBuy);
}

TEST(BinanceDataSessionTest, PublishesTradeToCombinedShm) {
  aquila::market_data::DataShmConfig shm_config{
      .enabled = true,
      .shm_name = UniqueShmName("trade_combined"),
      .book_ticker_channel_name = "book_ticker_channel",
      .trade_channel_name = "trade_channel",
      .create = true,
      .remove_existing = true,
  };
  ShmCleanup cleanup(shm_config.shm_name);

  aquila::market_data::DataShmPublisher publisher(shm_config);
  aquila::market_data::TradeShmReader reader(shm_config.TradeConfigForAttach());
  reader.SeekLatest();

  aquila::binance::DataSessionConfig config{};
  config.name = "binance_trade_shm";
  config.connection.host = "fstream.binance.com";
  config.connection.port = "443";
  config.exchange_symbols = {"BTCUSDT"};
  config.symbol_ids = {11};
  config.feeds = {.book_ticker = false, .trade = true};
  using ShmSession = aquila::binance::DataSession<
      aquila::market_data::DataShmPublisher,
      aquila::binance::DefaultTlsWebSocketPolicy,
      aquila::binance::SessionOnlyDiagnosticsPolicy>;
  ShmSession session(std::move(config), publisher);

  const auto result = session.Handle(TextView(kTradeJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(publisher.published_trades(), 1U);
  aquila::Trade trade{};
  ASSERT_TRUE(reader.TryReadOne(&trade));
  EXPECT_EQ(trade.symbol_id, 11);
  EXPECT_EQ(trade.exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(trade.id, 7868321828LL);
  EXPECT_EQ(trade.side, aquila::OrderSide::kBuy);
  EXPECT_EQ(trade.exchange_ns, 1783228448495LL * 1'000'000LL);
  EXPECT_EQ(trade.trade_ns, 1783228448495LL * 1'000'000LL);
  EXPECT_DOUBLE_EQ(trade.price, 62738.70);
  EXPECT_DOUBLE_EQ(trade.volume, 0.002);
  EXPECT_EQ(trade.batch_index, 0U);
  EXPECT_EQ(trade.batch_count, 1U);
}

TEST(BinanceDataSessionTest, DropsTradeWhenBookTickerOnlyShmPublisher) {
  aquila::market_data::BookTickerShmConfig shm_config{
      .enabled = true,
      .shm_name = UniqueShmName("book_only_disabled_trade"),
      .channel_name = "book_ticker_channel",
      .create = true,
      .remove_existing = true,
  };
  ShmCleanup cleanup(shm_config.shm_name);

  aquila::market_data::DataShmPublisher publisher(shm_config);
  aquila::binance::DataSessionConfig config{};
  config.name = "binance_book_only_shm";
  config.connection.host = "fstream.binance.com";
  config.connection.port = "443";
  config.exchange_symbols = {"BTCUSDT"};
  config.symbol_ids = {11};
  config.feeds = {.book_ticker = false, .trade = true};
  using ShmSession = aquila::binance::DataSession<
      aquila::market_data::DataShmPublisher,
      aquila::binance::DefaultTlsWebSocketPolicy,
      aquila::binance::DataSessionDiagnosticsPolicy>;
  ShmSession session(std::move(config), publisher);

  const auto result = session.Handle(TextView(kTradeJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(publisher.published_trades(), 0U);
  EXPECT_EQ(session.stats().trade_messages, 0U);
  EXPECT_EQ(session.market_data_client_diagnostics()
                .stats()
                .unsupported_event_messages,
            1U);
}

TEST(BinanceDataSessionTest, DropsBookTickerWhenTradeOnlyShmPublisher) {
  aquila::market_data::TradeShmConfig shm_config{
      .enabled = true,
      .shm_name = UniqueShmName("trade_only_disabled_book_ticker"),
      .channel_name = "trade_channel",
      .create = true,
      .remove_existing = true,
  };
  ShmCleanup cleanup(shm_config.shm_name);

  aquila::market_data::DataShmPublisher publisher(shm_config);
  aquila::binance::DataSessionConfig config{};
  config.name = "binance_trade_only_shm";
  config.connection.host = "fstream.binance.com";
  config.connection.port = "443";
  config.exchange_symbols = {"BTCUSDT"};
  config.symbol_ids = {11};
  config.feeds = {.book_ticker = true, .trade = false};
  using ShmSession = aquila::binance::DataSession<
      aquila::market_data::DataShmPublisher,
      aquila::binance::DefaultTlsWebSocketPolicy,
      aquila::binance::DataSessionDiagnosticsPolicy>;
  ShmSession session(std::move(config), publisher);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(publisher.published_book_tickers(), 0U);
  EXPECT_EQ(session.stats().book_ticker_messages, 0U);
  EXPECT_EQ(session.market_data_client_diagnostics()
                .stats()
                .unsupported_event_messages,
            1U);
}

TEST(BinanceDataSessionTest, IgnoresBinaryPayload) {
  RecordingDataSink data_sink;
  Session session = MakeSession(data_sink);

  const auto result = session.Handle(BinaryView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.stats().binary_messages, 1U);
  EXPECT_EQ(data_sink.book_ticker_calls, 0);
  EXPECT_EQ(data_sink.trade_calls, 0);
}

TEST(BinanceDataSessionTest, DefaultSessionDiagnosticsDoNotCount) {
  RecordingDataSink data_sink;
  DefaultNoStatsSession session = MakeDefaultNoStatsSession(data_sink);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(data_sink.book_ticker_calls, 1);
  EXPECT_EQ(session.stats().text_messages, 0U);
  EXPECT_EQ(session.stats().book_ticker_messages, 0U);
}

TEST(BinanceDataSessionTest, ExposesMarketDataDiagnosticsWhenEnabled) {
  RecordingDataSink data_sink;
  DiagnosticSession session = MakeDiagnosticSession(data_sink);

  const auto result = session.Handle(TextView(kBookTickerJson));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(data_sink.book_ticker_calls, 1);
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
