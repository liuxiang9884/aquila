#include "exchange/bitget/sbe/trade_decoder.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/market_data/types.h"
#include "evaluation/exchange/bitget/sbe/trade_payload_builder.h"
#include "exchange/bitget/sbe/message_dispatcher.h"

namespace {

TEST(BitgetSbeTradeDecoderTest, DecodesSingleBuyTrade) {
  std::array<char, 256> buffer{};
  const std::array<aquila::bitget::evaluation::PublicTradePayloadEntry, 1>
      entries{{
          {.ts = 1'700'000'000'000'002,
           .exec_id = 9999,
           .price = 6'566'738,
           .size = 5'000,
           .side = 0},
      }};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildPublicTradePayload(&buffer, "BTCUSDT",
                                                          entries);

  const aquila::bitget::SbeDispatchResult dispatch =
      aquila::bitget::DispatchSbeMessage(payload);
  ASSERT_EQ(dispatch.status, aquila::bitget::SbeDispatchStatus::kReady);
  ASSERT_EQ(dispatch.message_type,
            aquila::bitget::BitgetSbeMessageType::kPublicTrade);
  EXPECT_EQ(aquila::bitget::ExtractTrustedTradeSymbol(payload, dispatch.header),
            "BTCUSDT");

  std::vector<aquila::Trade> trades;
  aquila::bitget::DecodeTradesWithHeader(
      payload, dispatch.header, 1'700'000'000'001'200'000, 42,
      [&](const aquila::Trade& trade) noexcept { trades.push_back(trade); });

  ASSERT_EQ(trades.size(), 1U);
  EXPECT_EQ(trades[0].id, 9999);
  EXPECT_EQ(trades[0].symbol_id, 42);
  EXPECT_EQ(trades[0].exchange, aquila::Exchange::kBitget);
  EXPECT_EQ(trades[0].side, aquila::OrderSide::kBuy);
  EXPECT_EQ(trades[0].reserved, 0);
  EXPECT_EQ(trades[0].exchange_ns, 1'700'000'000'001'002'000);
  EXPECT_EQ(trades[0].event_ns, 1'700'000'000'000'002'000);
  EXPECT_EQ(trades[0].local_ns, 1'700'000'000'001'200'000);
  EXPECT_DOUBLE_EQ(trades[0].price, 65'667.38);
  EXPECT_DOUBLE_EQ(trades[0].volume, 0.5);
  EXPECT_EQ(trades[0].batch_index, 0U);
  EXPECT_EQ(trades[0].batch_count, 1U);
}

TEST(BitgetSbeTradeDecoderTest, DecodesSellSideAndBatchFields) {
  std::array<char, 512> buffer{};
  const std::array<aquila::bitget::evaluation::PublicTradePayloadEntry, 2>
      entries{{
          {.ts = 1'700'000'000'000'002,
           .exec_id = 10001,
           .price = 6'566'578,
           .size = 5'000,
           .side = 0},
          {.ts = 1'700'000'000'000'003,
           .exec_id = 10002,
           .price = 6'566'600,
           .size = 12'000,
           .side = 1},
      }};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildPublicTradePayload(&buffer, "BTCUSDT",
                                                          entries);
  const aquila::bitget::SbeDispatchResult dispatch =
      aquila::bitget::DispatchSbeMessage(payload);

  std::vector<aquila::Trade> trades;
  aquila::bitget::DecodeTradesWithHeader(
      payload, dispatch.header, 1'700'000'000'001'200'000, 42,
      [&](const aquila::Trade& trade) noexcept { trades.push_back(trade); });

  ASSERT_EQ(trades.size(), 2U);
  EXPECT_EQ(trades[0].id, 10001);
  EXPECT_EQ(trades[0].side, aquila::OrderSide::kBuy);
  EXPECT_DOUBLE_EQ(trades[0].price, 65'665.78);
  EXPECT_DOUBLE_EQ(trades[0].volume, 0.5);
  EXPECT_EQ(trades[0].batch_index, 0U);
  EXPECT_EQ(trades[0].batch_count, 2U);
  EXPECT_EQ(trades[1].id, 10002);
  EXPECT_EQ(trades[1].side, aquila::OrderSide::kSell);
  EXPECT_DOUBLE_EQ(trades[1].price, 65'666.0);
  EXPECT_DOUBLE_EQ(trades[1].volume, 1.2);
  EXPECT_EQ(trades[1].batch_index, 1U);
  EXPECT_EQ(trades[1].batch_count, 2U);
}

TEST(BitgetSbeTradeDecoderTest, DecodesEntryLevelStsWhenPresent) {
  std::array<char, 256> buffer{};
  const std::array<aquila::bitget::evaluation::PublicTradePayloadEntry, 1>
      entries{{
          {.ts = 1'700'000'000'000'002,
           .exec_id = 9999,
           .price = 6'566'738,
           .size = 5'000,
           .side = 0,
           .sts = 1'700'000'000'001'777},
      }};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildPublicTradePayload(
          &buffer, "BTCUSDT", entries,
          aquila::bitget::evaluation::PublicTradePayloadOptions{
              .block_length = 8,
              .schema_version = aquila::bitget::kBitgetSbeSchemaVersion,
              .entry_block_length = 42,
          });
  const aquila::bitget::SbeDispatchResult dispatch =
      aquila::bitget::DispatchSbeMessage(payload);

  std::vector<aquila::Trade> trades;
  aquila::bitget::DecodeTradesWithHeader(
      payload, dispatch.header, 1'700'000'000'001'200'000, 42,
      [&](const aquila::Trade& trade) noexcept { trades.push_back(trade); });

  ASSERT_EQ(trades.size(), 1U);
  EXPECT_EQ(trades[0].exchange_ns, 1'700'000'000'001'777'000);
}

TEST(BitgetSbeTradeDecoderTest, DecodesDirectlyIntoProvidedSlots) {
  std::array<char, 512> buffer{};
  const std::array<aquila::bitget::evaluation::PublicTradePayloadEntry, 2>
      entries{{
          {.ts = 1'700'000'000'000'002,
           .exec_id = 10001,
           .price = 6'566'578,
           .size = 5'000,
           .side = 0},
          {.ts = 1'700'000'000'000'003,
           .exec_id = 10002,
           .price = 6'566'600,
           .size = 12'000,
           .side = 1},
      }};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildPublicTradePayload(&buffer, "BTCUSDT",
                                                          entries);
  const aquila::bitget::SbeDispatchResult dispatch =
      aquila::bitget::DispatchSbeMessage(payload);

  std::array<aquila::Trade, 2> slots{};
  std::size_t slot_index{0};
  aquila::bitget::DecodeTradesWithHeaderToSlots(payload, dispatch.header,
                                                1'700'000'000'001'200'000, 42,
                                                [&](auto&& fill_slot) noexcept {
                                                  fill_slot(slots[slot_index]);
                                                  ++slot_index;
                                                });

  ASSERT_EQ(slot_index, 2U);
  EXPECT_EQ(slots[0].id, 10001);
  EXPECT_EQ(slots[0].batch_index, 0U);
  EXPECT_EQ(slots[0].batch_count, 2U);
  EXPECT_EQ(slots[1].id, 10002);
  EXPECT_EQ(slots[1].side, aquila::OrderSide::kSell);
  EXPECT_DOUBLE_EQ(slots[1].volume, 1.2);
}

TEST(BitgetSbeTradeDecoderTest, RejectsShortPayloadForTestHelper) {
  std::array<char, 256> buffer{};
  const std::array<aquila::bitget::evaluation::PublicTradePayloadEntry, 1>
      entries{{
          {.ts = 1'700'000'000'000'002,
           .exec_id = 9999,
           .price = 6'566'738,
           .size = 5'000,
           .side = 0},
      }};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildPublicTradePayload(&buffer, "BTCUSDT",
                                                          entries);
  const aquila::bitget::SbeDispatchResult dispatch =
      aquila::bitget::DispatchSbeMessage(payload);

  EXPECT_FALSE(aquila::bitget::detail::HasCompleteTradePayload(
      payload.substr(0, payload.size() - 1), dispatch.header));
}

}  // namespace
