#include "exchange/binance/market_data/trade_parser.h"

#include <array>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include <simdjson.h>

namespace {

constexpr std::string_view kTradeJson =
    R"({"e":"trade","E":1783228448495,"T":1783228448495,"s":"BTCUSDT","t":7868321828,"p":"62738.70","q":"0.002","X":"MARKET","m":false,"st":1})";

TEST(BinanceTradeParserTest, ParsesLiveProbedRawTradeJson) {
  simdjson::ondemand::parser parser;
  aquila::binance::TradeUpdate update{};

  const auto status =
      aquila::binance::ParseTrade(kTradeJson, 0, parser, update);

  EXPECT_EQ(status, aquila::binance::TradeParseStatus::kOk);
  EXPECT_EQ(update.symbol, "BTCUSDT");
  EXPECT_EQ(update.symbol.data(), update.symbol_storage.data());
  EXPECT_EQ(update.trade_id, 7868321828LL);
  EXPECT_EQ(update.event_time_ms, 1783228448495LL);
  EXPECT_EQ(update.trade_time_ms, 1783228448495LL);
  EXPECT_DOUBLE_EQ(update.price, 62738.70);
  EXPECT_DOUBLE_EQ(update.volume, 0.002);
  EXPECT_FALSE(update.buyer_is_maker);
}

TEST(BinanceTradeParserTest, ParsesPaddedViewWithoutCopy) {
  std::array<char, kTradeJson.size() + simdjson::SIMDJSON_PADDING> buffer{};
  std::memcpy(buffer.data(), kTradeJson.data(), kTradeJson.size());
  simdjson::ondemand::parser parser;
  aquila::binance::TradeUpdate update{};

  const auto status = aquila::binance::ParseTrade(
      std::string_view(buffer.data(), kTradeJson.size()),
      simdjson::SIMDJSON_PADDING, parser, update);

  EXPECT_EQ(status, aquila::binance::TradeParseStatus::kOk);
  EXPECT_EQ(update.symbol, "BTCUSDT");
  EXPECT_EQ(update.symbol.data(), update.symbol_storage.data());
  EXPECT_EQ(update.trade_id, 7868321828LL);
}

TEST(BinanceTradeParserTest, ParsesBuyerMakerAsSellTakerInput) {
  static constexpr std::string_view kBuyerMakerTradeJson =
      R"({"e":"trade","E":1783228448434,"T":1783228448434,"s":"ETHUSDT","t":8412341979,"p":"1765.20","q":"0.109","X":"MARKET","m":true,"st":1})";
  simdjson::ondemand::parser parser;
  aquila::binance::TradeUpdate update{};

  const auto status =
      aquila::binance::ParseTrade(kBuyerMakerTradeJson, 0, parser, update);

  EXPECT_EQ(status, aquila::binance::TradeParseStatus::kOk);
  EXPECT_EQ(update.symbol, "ETHUSDT");
  EXPECT_TRUE(update.buyer_is_maker);
}

TEST(BinanceTradeParserTest, RejectsMalformedJson) {
  static constexpr std::string_view kInvalidJson =
      R"({"e":"trade","E":1783228448495,)";
  simdjson::ondemand::parser parser;
  aquila::binance::TradeUpdate update{};

  const auto status =
      aquila::binance::ParseTrade(kInvalidJson, 0, parser, update);

  EXPECT_EQ(status, aquila::binance::TradeParseStatus::kMalformedJson);
}

TEST(BinanceTradeParserTest, RejectsMissingTradeField) {
  static constexpr std::string_view kMissingQuantity =
      R"({"e":"trade","E":1783228448495,"T":1783228448495,"s":"BTCUSDT","t":7868321828,"p":"62738.70","m":false})";
  simdjson::ondemand::parser parser;
  aquila::binance::TradeUpdate update{};

  const auto status =
      aquila::binance::ParseTrade(kMissingQuantity, 0, parser, update);

  EXPECT_EQ(status, aquila::binance::TradeParseStatus::kMalformedJson);
}

TEST(BinanceTradeParserTest, RejectsWrongTradeFieldType) {
  static constexpr std::string_view kWrongMakerType =
      R"({"e":"trade","E":1783228448495,"T":1783228448495,"s":"BTCUSDT","t":7868321828,"p":"62738.70","q":"0.002","m":"false"})";
  simdjson::ondemand::parser parser;
  aquila::binance::TradeUpdate update{};

  const auto status =
      aquila::binance::ParseTrade(kWrongMakerType, 0, parser, update);

  EXPECT_EQ(status, aquila::binance::TradeParseStatus::kMalformedJson);
}

TEST(BinanceTradeParserTest, RejectsOversizedSymbol) {
  static constexpr std::string_view kOversizedSymbol =
      R"({"e":"trade","E":1783228448495,"T":1783228448495,"s":"BTCUSDTBTCUSDTBTCUSDTBTCUSDTBTCUSDT","t":7868321828,"p":"62738.70","q":"0.002","m":false})";
  simdjson::ondemand::parser parser;
  aquila::binance::TradeUpdate update{};

  const auto status =
      aquila::binance::ParseTrade(kOversizedSymbol, 0, parser, update);

  EXPECT_EQ(status, aquila::binance::TradeParseStatus::kMalformedJson);
}

}  // namespace
