#include "exchange/binance/market_data/book_ticker_parser.h"

#include <array>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include <simdjson.h>

namespace {

constexpr std::string_view kBookTickerJson =
    R"({"e":"bookTicker","u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";

TEST(BinanceBookTickerParserTest, ParsesOfficialBookTickerJson) {
  simdjson::ondemand::parser parser;
  aquila::binance::BookTickerUpdate update{};

  const auto status =
      aquila::binance::ParseBookTicker(kBookTickerJson, 0, parser, update);

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

TEST(BinanceBookTickerParserTest, ParsesPaddedViewWithoutCopy) {
  std::array<char, kBookTickerJson.size() + simdjson::SIMDJSON_PADDING>
      buffer{};
  std::memcpy(buffer.data(), kBookTickerJson.data(), kBookTickerJson.size());
  simdjson::ondemand::parser parser;
  aquila::binance::BookTickerUpdate update{};

  const auto status = aquila::binance::ParseBookTicker(
      std::string_view(buffer.data(), kBookTickerJson.size()),
      simdjson::SIMDJSON_PADDING, parser, update);

  EXPECT_EQ(status, aquila::binance::BookTickerParseStatus::kOk);
  EXPECT_EQ(update.symbol, "BTCUSDT");
  EXPECT_EQ(update.symbol.data(), update.symbol_storage.data());
  EXPECT_EQ(update.update_id, 400900217);
}

TEST(BinanceBookTickerParserTest, PaddedViewKeepsSymbolInsideUpdateStorage) {
  std::array<char, kBookTickerJson.size() + simdjson::SIMDJSON_PADDING>
      buffer{};
  std::memcpy(buffer.data(), kBookTickerJson.data(), kBookTickerJson.size());
  simdjson::ondemand::parser parser;
  aquila::binance::BookTickerUpdate update{};

  ASSERT_EQ(aquila::binance::ParseBookTicker(
                std::string_view(buffer.data(), kBookTickerJson.size()),
                simdjson::SIMDJSON_PADDING, parser, update),
            aquila::binance::BookTickerParseStatus::kOk);

  EXPECT_EQ(update.symbol, "BTCUSDT");
  EXPECT_EQ(update.symbol.data(), update.symbol_storage.data());
}

TEST(BinanceBookTickerParserTest, IgnoresEventFieldOnRawBookTickerStream) {
  static constexpr std::string_view kAggTradeJson =
      R"({"e":"aggTrade","u":400900217,"E":1568014460893,"T":1568014460891,"s":"BTCUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})";
  simdjson::ondemand::parser parser;
  aquila::binance::BookTickerUpdate update{};

  const auto status =
      aquila::binance::ParseBookTicker(kAggTradeJson, 0, parser, update);

  EXPECT_EQ(status, aquila::binance::BookTickerParseStatus::kOk);
  EXPECT_EQ(update.symbol, "BTCUSDT");
  EXPECT_EQ(update.update_id, 400900217);
}

TEST(BinanceBookTickerParserTest, RejectsMalformedJson) {
  static constexpr std::string_view kInvalidJson =
      R"({"e":"bookTicker","u":400900217,"E":1568014460893,)";
  simdjson::ondemand::parser parser;
  aquila::binance::BookTickerUpdate update{};

  const auto status =
      aquila::binance::ParseBookTicker(kInvalidJson, 0, parser, update);

  EXPECT_EQ(status, aquila::binance::BookTickerParseStatus::kMalformedJson);
}

}  // namespace
