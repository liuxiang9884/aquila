#include "exchange/gate/sbe/trade_decoder.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/market_data/types.h"
#include "evaluation/exchange/gate/sbe/trade_payload_builder.h"
#include "exchange/gate/sbe/message_dispatcher.h"

namespace {

TEST(GateSbeTradeDecoderTest, DecodesSingleBuyTrade) {
  std::array<char, 256> buffer{};
  const std::array<aquila::gate::evaluation::PublicTradePayloadEntry, 1>
      entries{{
          {.t = 1'770'000'000'000'990,
           .id = 123456789,
           .size = 17'500,
           .price = 650'125'000},
      }};
  const std::string_view payload =
      aquila::gate::evaluation::BuildPublicTradePayload(&buffer, "BTC_USDT",
                                                        entries);

  const aquila::gate::SbeDispatchResult dispatch =
      aquila::gate::DispatchSbeMessage(payload);
  ASSERT_EQ(dispatch.status, aquila::gate::SbeDispatchStatus::kReady);
  ASSERT_EQ(dispatch.message_type,
            aquila::gate::GateSbeMessageType::kPublicTrade);
  EXPECT_EQ(aquila::gate::ExtractTrustedTradeSymbol(payload, dispatch.header),
            "BTC_USDT");

  std::vector<aquila::Trade> trades;
  aquila::gate::DecodeTradesWithHeader(
      payload, dispatch.header, 1'770'000'000'001'200'000, 42,
      [&](const aquila::Trade& trade) noexcept { trades.push_back(trade); });

  ASSERT_EQ(trades.size(), 1U);
  EXPECT_EQ(trades[0].id, 123456789);
  EXPECT_EQ(trades[0].symbol_id, 42);
  EXPECT_EQ(trades[0].exchange, aquila::Exchange::kGate);
  EXPECT_EQ(trades[0].side, aquila::OrderSide::kBuy);
  EXPECT_EQ(trades[0].reserved, 0);
  EXPECT_EQ(trades[0].exchange_ns, 1'770'000'000'001'000'000);
  EXPECT_EQ(trades[0].event_ns, 1'770'000'000'000'990'000);
  EXPECT_EQ(trades[0].local_ns, 1'770'000'000'001'200'000);
  EXPECT_DOUBLE_EQ(trades[0].price, 65'012.5);
  EXPECT_DOUBLE_EQ(trades[0].volume, 17.5);
  EXPECT_EQ(trades[0].batch_index, 0U);
  EXPECT_EQ(trades[0].batch_count, 1U);
}

TEST(GateSbeTradeDecoderTest, DecodesSellSideAndBatchFields) {
  std::array<char, 512> buffer{};
  const std::array<aquila::gate::evaluation::PublicTradePayloadEntry, 2>
      entries{{
          {.t = 1'770'000'000'000'990,
           .id = 123456789,
           .size = 17'500,
           .price = 650'125'000},
          {.t = 1'770'000'000'000'991,
           .id = 123456790,
           .size = -21'000,
           .price = 650'120'000},
      }};
  const std::string_view payload =
      aquila::gate::evaluation::BuildPublicTradePayload(&buffer, "BTC_USDT",
                                                        entries);

  const aquila::gate::SbeDispatchResult dispatch =
      aquila::gate::DispatchSbeMessage(payload);
  std::vector<aquila::Trade> trades;
  aquila::gate::DecodeTradesWithHeader(
      payload, dispatch.header, 1'770'000'000'001'200'000, 42,
      [&](const aquila::Trade& trade) noexcept { trades.push_back(trade); });

  ASSERT_EQ(trades.size(), 2U);
  EXPECT_EQ(trades[0].side, aquila::OrderSide::kBuy);
  EXPECT_DOUBLE_EQ(trades[0].volume, 17.5);
  EXPECT_EQ(trades[0].batch_index, 0U);
  EXPECT_EQ(trades[0].batch_count, 2U);
  EXPECT_EQ(trades[1].side, aquila::OrderSide::kSell);
  EXPECT_DOUBLE_EQ(trades[1].volume, 21.0);
  EXPECT_EQ(trades[1].batch_index, 1U);
  EXPECT_EQ(trades[1].batch_count, 2U);
}

TEST(GateSbeTradeDecoderTest, DecodesDirectlyIntoProvidedSlots) {
  std::array<char, 512> buffer{};
  const std::array<aquila::gate::evaluation::PublicTradePayloadEntry, 2>
      entries{{
          {.t = 1'770'000'000'000'990,
           .id = 123456789,
           .size = 17'500,
           .price = 650'125'000},
          {.t = 1'770'000'000'000'991,
           .id = 123456790,
           .size = -21'000,
           .price = 650'120'000},
      }};
  const std::string_view payload =
      aquila::gate::evaluation::BuildPublicTradePayload(&buffer, "BTC_USDT",
                                                        entries);
  const aquila::gate::SbeDispatchResult dispatch =
      aquila::gate::DispatchSbeMessage(payload);

  std::array<aquila::Trade, 2> slots{};
  std::size_t slot_index{0};
  aquila::gate::DecodeTradesWithHeaderToSlots(payload, dispatch.header,
                                              1'770'000'000'001'200'000, 42,
                                              [&](auto&& fill_slot) noexcept {
                                                fill_slot(slots[slot_index]);
                                                ++slot_index;
                                              });

  ASSERT_EQ(slot_index, 2U);
  EXPECT_EQ(slots[0].id, 123456789);
  EXPECT_EQ(slots[0].batch_index, 0U);
  EXPECT_EQ(slots[0].batch_count, 2U);
  EXPECT_EQ(slots[1].id, 123456790);
  EXPECT_EQ(slots[1].side, aquila::OrderSide::kSell);
  EXPECT_DOUBLE_EQ(slots[1].price, 65'012.0);
}

}  // namespace
