#include "exchange/bitget/trading/fast_fill_parser.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <simdjson.h>

namespace aquila::bitget {
namespace {

struct OwnedFastFillRecord {
  std::string category;
  std::string symbol;
  std::string order_id;
  std::string client_oid;
  std::string exec_id;
  std::string side;
  std::string hold_side;
  std::string exec_price;
  std::string exec_quantity;
  std::string trade_scope;
  std::uint64_t exchange_message_time_ms{0};
  std::uint64_t exec_time_ms{0};
  std::uint64_t updated_time_ms{0};
};

OwnedFastFillRecord Own(const FastFillRecord& record) {
  return {
      .category = std::string(record.category),
      .symbol = std::string(record.symbol),
      .order_id = std::string(record.order_id),
      .client_oid = std::string(record.client_oid),
      .exec_id = std::string(record.exec_id),
      .side = std::string(record.side),
      .hold_side = std::string(record.hold_side),
      .exec_price = std::string(record.exec_price),
      .exec_quantity = std::string(record.exec_quantity),
      .trade_scope = std::string(record.trade_scope),
      .exchange_message_time_ms = record.exchange_message_time_ms,
      .exec_time_ms = record.exec_time_ms,
      .updated_time_ms = record.updated_time_ms,
  };
}

struct ParseOutput {
  FastFillParseResult result;
  FastFillParserStats stats;
  std::vector<OwnedFastFillRecord> records;
};

ParseOutput Parse(std::string_view payload) {
  ParseOutput output;
  simdjson::ondemand::parser parser;
  output.result = ParseBitgetFastFillMessage(
      payload, /*readable_tail_bytes=*/0, parser, output.stats,
      [&output](const FastFillRecord& record) noexcept {
        output.records.push_back(Own(record));
      });
  return output;
}

constexpr std::string_view kFastFill = R"({
  "data":{
    "symbol":"BTCUSDT",
    "updatedTime":"1736378720623",
    "side":"buy",
    "orderId":"1288888888888888888",
    "execTime":"1736378720621",
    "tradeScope":"taker",
    "execId":"1288888888888888890",
    "execPrice":"94993",
    "holdSide":"long",
    "category":"usdt-futures",
    "execQty":"0.01",
    "clientOid":"a-72057594037927978"
  },
  "arg":{"instType":"UTA","topic":"fast-fill"},
  "action":"update",
  "ts":1736378720653
})";

TEST(BitgetFastFillParserTest, PreservesDocumentedRawFields) {
  const ParseOutput output = Parse(kFastFill);

  EXPECT_EQ(output.result.status, FastFillParseStatus::kOk);
  EXPECT_EQ(output.stats.messages_seen, 1U);
  EXPECT_EQ(output.stats.records_emitted, 1U);
  ASSERT_EQ(output.records.size(), 1U);
  const OwnedFastFillRecord& record = output.records.front();
  EXPECT_EQ(record.category, "usdt-futures");
  EXPECT_EQ(record.symbol, "BTCUSDT");
  EXPECT_EQ(record.order_id, "1288888888888888888");
  EXPECT_EQ(record.client_oid, "a-72057594037927978");
  EXPECT_EQ(record.exec_id, "1288888888888888890");
  EXPECT_EQ(record.side, "buy");
  EXPECT_EQ(record.hold_side, "long");
  EXPECT_EQ(record.exec_price, "94993");
  EXPECT_EQ(record.exec_quantity, "0.01");
  EXPECT_EQ(record.trade_scope, "taker");
  EXPECT_EQ(record.exchange_message_time_ms, 1'736'378'720'653ULL);
  EXPECT_EQ(record.exec_time_ms, 1'736'378'720'621ULL);
  EXPECT_EQ(record.updated_time_ms, 1'736'378'720'623ULL);
}

TEST(BitgetFastFillParserTest, RejectsWrongTopicAndMissingRequiredField) {
  std::string wrong_topic{kFastFill};
  wrong_topic.replace(wrong_topic.find("fast-fill"), 9, "fill");
  ParseOutput output = Parse(wrong_topic);
  EXPECT_EQ(output.result.status, FastFillParseStatus::kUnexpectedEnvelope);
  EXPECT_TRUE(output.records.empty());

  std::string missing_exec_id{kFastFill};
  const std::string field = R"("execId":"1288888888888888890",)";
  missing_exec_id.erase(missing_exec_id.find(field), field.size());
  output = Parse(missing_exec_id);
  EXPECT_EQ(output.result.status, FastFillParseStatus::kDecodeUnrecoverable);
  EXPECT_TRUE(output.records.empty());
}

TEST(BitgetFastFillParserTest, DistinguishesMalformedJson) {
  const ParseOutput output = Parse(R"({"action":"update")");

  EXPECT_EQ(output.result.status, FastFillParseStatus::kInvalidJson);
  EXPECT_EQ(output.stats.invalid_json_count, 1U);
  EXPECT_TRUE(output.records.empty());
}

}  // namespace
}  // namespace aquila::bitget
