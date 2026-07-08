#include "exchange/bitget/sbe/book_ticker_decoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "evaluation/exchange/bitget/sbe/book_ticker_payload_builder.h"

namespace {

bool DecodeBookTickerForTest(std::string_view payload, std::int64_t local_ns,
                             std::int32_t symbol_id,
                             aquila::BookTicker* out) noexcept {
  const aquila::bitget::SbeDispatchResult dispatch =
      aquila::bitget::DispatchSbeMessage(payload);
  if (out == nullptr ||
      dispatch.status != aquila::bitget::SbeDispatchStatus::kReady ||
      dispatch.message_type !=
          aquila::bitget::BitgetSbeMessageType::kBookTicker ||
      !aquila::bitget::detail::HasCompleteBookTickerPayload(payload,
                                                            dispatch.header)) {
    return false;
  }

  aquila::bitget::DecodeBookTickerWithHeader(payload, dispatch.header, local_ns,
                                             symbol_id, *out);
  return true;
}

std::string_view ExtractBookTickerSymbolForTest(
    std::string_view payload) noexcept {
  const aquila::bitget::SbeDispatchResult dispatch =
      aquila::bitget::DispatchSbeMessage(payload);
  if (dispatch.status != aquila::bitget::SbeDispatchStatus::kReady ||
      dispatch.message_type !=
          aquila::bitget::BitgetSbeMessageType::kBookTicker ||
      !aquila::bitget::detail::HasCompleteBookTickerPayload(payload,
                                                            dispatch.header)) {
    return {};
  }

  return aquila::bitget::ExtractTrustedBookTickerSymbol(payload,
                                                        dispatch.header);
}

}  // namespace

TEST(BitgetSbeBookTickerDecoderTest, MapsBooks1ToBookTicker) {
  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(
          &buffer, "BTCUSDT",
          aquila::bitget::evaluation::BookTickerPayloadOptions{
              .block_length = 59,
              .ts = 1'700'000'000'000'001,
              .bid1_price = 6'569'038,
              .bid1_size = 15'000,
              .ask1_price = 6'569'042,
              .ask1_size = 20'000,
              .price_exponent = -2,
              .size_exponent = -4,
              .seq = 42,
              .sts = 1'700'000'000'001'001,
              .category = 1,
          });

  const aquila::bitget::SbeDispatchResult dispatch =
      aquila::bitget::DispatchSbeMessage(payload);
  ASSERT_EQ(dispatch.status, aquila::bitget::SbeDispatchStatus::kReady);

  aquila::BookTicker ticker{};
  aquila::bitget::DecodeBookTickerWithHeader(
      payload, dispatch.header, 1'700'000'000'002'003'000, 7, ticker);

  EXPECT_EQ(ticker.id, 42);
  EXPECT_EQ(ticker.symbol_id, 7);
  EXPECT_EQ(ticker.exchange, aquila::Exchange::kBitget);
  EXPECT_EQ(ticker.exchange_ns, 1'700'000'000'001'001'000);
  EXPECT_EQ(ticker.event_ns, 1'700'000'000'000'001'000);
  EXPECT_EQ(ticker.local_ns, 1'700'000'000'002'003'000);
  EXPECT_DOUBLE_EQ(ticker.bid_price, 65'690.38);
  EXPECT_DOUBLE_EQ(ticker.bid_volume, 1.5);
  EXPECT_DOUBLE_EQ(ticker.ask_price, 65'690.42);
  EXPECT_DOUBLE_EQ(ticker.ask_volume, 2.0);
}

TEST(BitgetSbeBookTickerDecoderTest, AcceptsLiveObservedBlockLength64) {
  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(
          &buffer, "BTCUSDT",
          aquila::bitget::evaluation::BookTickerPayloadOptions{
              .block_length = 64,
          });

  aquila::BookTicker ticker{};

  ASSERT_TRUE(DecodeBookTickerForTest(payload, 999'000, 11, &ticker));
  EXPECT_EQ(ticker.id, 42);
  EXPECT_EQ(ticker.symbol_id, 11);
  EXPECT_EQ(ticker.exchange, aquila::Exchange::kBitget);
}

TEST(BitgetSbeBookTickerDecoderTest, AcceptsLiveSchemaVersion3) {
  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(
          &buffer, "BTCUSDT",
          aquila::bitget::evaluation::BookTickerPayloadOptions{
              .block_length = 64,
              .schema_version = aquila::bitget::kBitgetSbeLiveSchemaVersion,
          });

  aquila::BookTicker ticker{};

  ASSERT_TRUE(DecodeBookTickerForTest(payload, 999'000, 11, &ticker));
  EXPECT_EQ(ticker.id, 42);
  EXPECT_EQ(ticker.symbol_id, 11);
  EXPECT_EQ(ticker.exchange, aquila::Exchange::kBitget);
}

TEST(BitgetSbeBookTickerDecoderTest,
     FallsBackExchangeTimeToEventTimeWithoutSts) {
  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(
          &buffer, "BTCUSDT",
          aquila::bitget::evaluation::BookTickerPayloadOptions{
              .block_length = 50,
              .ts = 1'700'000'000'000'001,
              .include_sts = false,
              .include_category = false,
          });

  aquila::BookTicker ticker{};

  ASSERT_TRUE(DecodeBookTickerForTest(payload, 999'000, 11, &ticker));
  EXPECT_EQ(ticker.event_ns, 1'700'000'000'000'001'000);
  EXPECT_EQ(ticker.exchange_ns, ticker.event_ns);
}

TEST(BitgetSbeBookTickerDecoderTest, ExtractsBookTickerSymbol) {
  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(&buffer, "ETHUSDT");

  EXPECT_EQ(ExtractBookTickerSymbolForTest(payload), "ETHUSDT");
}

TEST(BitgetSbeBookTickerDecoderTest, RejectsShortPayloadForTestHelper) {
  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(&buffer, "BTCUSDT");

  aquila::BookTicker ticker{};

  EXPECT_FALSE(DecodeBookTickerForTest(payload.substr(0, payload.size() - 1),
                                       999'000, 11, &ticker));
  EXPECT_EQ(
      ExtractBookTickerSymbolForTest(payload.substr(0, payload.size() - 1)),
      "");
}
