#include "exchange/binance/market_data/book_ticker_yyjson_parser.h"

#include <array>
#include <cstring>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

namespace {

constexpr std::string_view kBookTickerJson =
    R"({"e":"bookTicker","u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";

TEST(BinanceBookTickerYyjsonParserTest, ParsesOfficialBookTickerJson) {
  aquila::binance::YyjsonBookTickerParser parser;
  aquila::binance::BookTickerUpdate update{};

  const auto status = parser.Parse(kBookTickerJson, 0, update);

  EXPECT_EQ(status, aquila::binance::BookTickerParseStatus::kOk);
  EXPECT_EQ(update.symbol, "BTCUSDT");
  EXPECT_EQ(update.symbol.data(), update.symbol_storage.data());
  EXPECT_EQ(update.update_id, 400900217);
  EXPECT_EQ(update.event_time_ms, 1568014460893);
  EXPECT_EQ(update.transaction_time_ms, 1568014460891);
  EXPECT_DOUBLE_EQ(update.bid_price, 25.3519);
  EXPECT_DOUBLE_EQ(update.bid_volume, 31.21);
  EXPECT_DOUBLE_EQ(update.ask_price, 25.3652);
  EXPECT_DOUBLE_EQ(update.ask_volume, 40.66);
}

TEST(BinanceBookTickerYyjsonParserTest, RejectsUnsupportedEvent) {
  static constexpr std::string_view kAggTradeJson =
      R"({"e":"aggTrade","u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";
  aquila::binance::YyjsonBookTickerParser parser;
  aquila::binance::BookTickerUpdate update{};

  const auto status = parser.Parse(kAggTradeJson, 0, update);

  EXPECT_EQ(status, aquila::binance::BookTickerParseStatus::kUnsupportedEvent);
}

TEST(BinanceBookTickerYyjsonParserTest, RejectsInvalidNumberString) {
  static constexpr std::string_view kInvalidJson =
      R"({"e":"bookTicker","u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"bad","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";
  aquila::binance::YyjsonBookTickerParser parser;
  aquila::binance::BookTickerUpdate update{};

  const auto status = parser.Parse(kInvalidJson, 0, update);

  EXPECT_EQ(status, aquila::binance::BookTickerParseStatus::kInvalidNumber);
}

TEST(BinanceBookTickerYyjsonParserTest, ParsesInsituPaddedMutablePayload) {
  std::array<char, kBookTickerJson.size() + YYJSON_PADDING_SIZE> buffer{};
  std::memcpy(buffer.data(), kBookTickerJson.data(), kBookTickerJson.size());
  aquila::binance::YyjsonBookTickerParser parser;
  aquila::binance::BookTickerUpdate update{};

  const auto status =
      parser.ParseInsitu(std::span<char>(buffer.data(), buffer.size()),
                         kBookTickerJson.size(), update);

  EXPECT_EQ(status, aquila::binance::BookTickerParseStatus::kOk);
  EXPECT_EQ(update.symbol, "BTCUSDT");
  EXPECT_EQ(update.update_id, 400900217);
  EXPECT_DOUBLE_EQ(update.bid_price, 25.3519);
}

TEST(BinanceBookTickerYyjsonParserTest, RejectsInsituWithoutYyjsonPadding) {
  std::array<char, kBookTickerJson.size() + YYJSON_PADDING_SIZE - 1U> buffer{};
  std::memcpy(buffer.data(), kBookTickerJson.data(), kBookTickerJson.size());
  aquila::binance::YyjsonBookTickerParser parser;
  aquila::binance::BookTickerUpdate update{};

  const auto status =
      parser.ParseInsitu(std::span<char>(buffer.data(), buffer.size()),
                         kBookTickerJson.size(), update);

  EXPECT_EQ(status, aquila::binance::BookTickerParseStatus::kMalformedJson);
}

}  // namespace
