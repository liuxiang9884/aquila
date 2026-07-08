#include "exchange/bitget/market_data/data_session.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "core/websocket/message_view.h"
#include "evaluation/exchange/bitget/sbe/book_ticker_payload_builder.h"

namespace {

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

struct RecordingDataSink {
  int calls{0};
  aquila::BookTicker last{};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++calls;
    last = book_ticker;
  }
};

using Session =
    aquila::bitget::DataSession<RecordingDataSink,
                                aquila::bitget::DefaultTlsWebSocketPolicy,
                                aquila::bitget::SessionOnlyDiagnosticsPolicy>;

static_assert(Session::SessionDiagnosticsEnabled);
static_assert(Session::kClockSource ==
              aquila::websocket::ClockSource::kRealtime);
static_assert(!std::is_constructible_v<
              Session, aquila::websocket::ConnectionConfig,
              std::span<const aquila::bitget::SymbolBinding>,
              RecordingDataSink&, aquila::websocket::ClockSource>);

template <typename SessionT>
concept HasRun = requires(SessionT& session) {
  { session.Run() } -> std::same_as<bool>;
};

static_assert(HasRun<Session>);

Session MakeSession(RecordingDataSink& data_sink) {
  static constexpr std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  aquila::websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.port = "443";
  config.target = "/v3/ws/public/sbe";
  return Session(std::move(config), symbols, data_sink, {}, "usdt-futures");
}

}  // namespace

TEST(BitgetDataSessionTest, SendsBooks1SubscribeWhenConnectionBecomesActive) {
  RecordingDataSink data_sink;
  Session session = MakeSession(data_sink);

  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kActive);

  EXPECT_EQ(session.subscription_state(),
            aquila::bitget::SubscriptionState::kSubscribeSent);
  EXPECT_EQ(session.subscribe_status(), aquila::websocket::SendStatus::kOk);
  EXPECT_EQ(session.stats().subscribe_sent, 1U);
  EXPECT_NE(session.last_subscribe_request().find("\"topic\":\"books1\""),
            std::string_view::npos);
  EXPECT_NE(session.last_subscribe_request().find("BTCUSDT"),
            std::string_view::npos);
}

TEST(BitgetDataSessionTest, HandlesSubscribeAckForBooks1) {
  RecordingDataSink data_sink;
  Session session = MakeSession(data_sink);

  const auto result = session.Handle(TextView(
      R"({"event":"subscribe","arg":{"instType":"usdt-futures","topic":"books1","symbol":"BTCUSDT"},"code":"0"})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(session.subscription_state(),
            aquila::bitget::SubscriptionState::kSubscribed);
  EXPECT_EQ(session.stats().text_messages, 1U);
  EXPECT_EQ(session.stats().control_messages, 1U);
  EXPECT_EQ(session.stats().subscribe_acks, 1U);
}

TEST(BitgetDataSessionTest, CapturesLocalNsBeforeDecode) {
  RecordingDataSink data_sink;
  Session session = MakeSession(data_sink);
  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(&buffer, "BTCUSDT");

  const auto result = session.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(data_sink.calls, 1);
  EXPECT_GT(data_sink.last.local_ns, 0);
  EXPECT_EQ(data_sink.last.exchange, aquila::Exchange::kBitget);
  EXPECT_EQ(session.stats().binary_messages, 1U);
}

TEST(BitgetDataSessionTest, PublishesBookTickerToSink) {
  RecordingDataSink data_sink;
  Session session = MakeSession(data_sink);
  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(&buffer, "BTCUSDT");

  const auto result = session.Handle(BinaryView(payload));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(data_sink.calls, 1);
  EXPECT_EQ(data_sink.last.symbol_id, 11);
  EXPECT_EQ(data_sink.last.id, 42);
  EXPECT_DOUBLE_EQ(data_sink.last.bid_price, 65'690.38);
}
