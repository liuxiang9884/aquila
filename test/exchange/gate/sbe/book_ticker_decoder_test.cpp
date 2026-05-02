#include "exchange/gate/sbe/book_ticker_decoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include "core/market_data/types.h"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"

namespace {

template <typename T>
void WriteLittleEndian(std::array<char, 128>& buffer, size_t offset,
                       T value) noexcept {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

void WriteVarString(std::array<char, 128>& buffer, size_t* offset,
                    std::string_view value) noexcept {
  buffer[(*offset)++] = static_cast<char>(value.size());
  std::memcpy(buffer.data() + *offset, value.data(), value.size());
  *offset += value.size();
}

double DecimalMantissaToDoubleForTest(std::int64_t mantissa,
                                      std::int8_t exponent) noexcept {
  return static_cast<double>(mantissa) *
         aquila::gate::detail::DecimalExponentScale(exponent);
}

std::string_view ExtractBookTickerSymbolForTest(
    std::string_view payload) noexcept {
  if (payload.size() < aquila::gate::detail::kMinBookTickerPayloadBytes) {
    return {};
  }

  size_t offset = aquila::gate::kSbeMessageHeaderBytes +
                  aquila::gate::detail::kBookTickerBlockLength;
  std::string_view channel;
  std::string_view symbol;
  if (!aquila::gate::detail::ReadVarString8(payload, offset, channel) ||
      !aquila::gate::detail::ReadVarString8(payload, offset, symbol)) {
    return {};
  }
  return symbol;
}

bool DecodeBookTickerWithHeaderForTest(
    std::string_view payload, const aquila::gate::SbeMessageHeader& header,
    std::int64_t local_ns, std::int32_t symbol_id,
    aquila::BookTicker* out) noexcept {
  if (out == nullptr ||
      payload.size() < aquila::gate::detail::kMinBookTickerPayloadBytes ||
      !aquila::gate::detail::IsBookTickerHeader(header)) {
    return false;
  }

  const auto view = ::sbepp::make_const_view<::gate::messages::bbo>(
      payload.data(), payload.size());
  if (view.e() != ::gate::types::Event::Update) {
    return false;
  }

  aquila::gate::detail::AssignBookTickerFromView(view, local_ns, symbol_id,
                                                 *out);
  return true;
}

bool DecodeBookTickerForTest(std::string_view payload, std::int64_t local_ns,
                             std::int32_t symbol_id,
                             aquila::BookTicker* out) noexcept {
  const aquila::gate::SbeDispatchResult dispatch =
      aquila::gate::DispatchSbeMessage(payload);
  if (dispatch.status != aquila::gate::SbeDispatchStatus::kReady ||
      dispatch.message_type != aquila::gate::GateSbeMessageType::kBookTicker) {
    return false;
  }

  return DecodeBookTickerWithHeaderForTest(payload, dispatch.header, local_ns,
                                           symbol_id, out);
}

}  // namespace

TEST(GateSbeBookTickerDecoderTest, ConvertsDecimalMantissaWithinFixedTable) {
  EXPECT_DOUBLE_EQ(DecimalMantissaToDoubleForTest(123, 0), 123.0);
  EXPECT_DOUBLE_EQ(DecimalMantissaToDoubleForTest(123, 10),
                   1'230'000'000'000.0);
  EXPECT_DOUBLE_EQ(DecimalMantissaToDoubleForTest(123, -10), 0.0000000123);
}

TEST(GateSbeBookTickerDecoderTest, ConvertsDecimalExponentToScale) {
  EXPECT_DOUBLE_EQ(aquila::gate::detail::DecimalExponentScale(0), 1.0);
  EXPECT_DOUBLE_EQ(aquila::gate::detail::DecimalExponentScale(10),
                   10'000'000'000.0);
  EXPECT_DOUBLE_EQ(aquila::gate::detail::DecimalExponentScale(-10),
                   0.0000000001);
}

TEST(GateSbeBookTickerDecoderTest, DecodesBookTickerFromBboPayload) {
  std::array<char, 128> buffer{};
  size_t offset = 0;
  WriteLittleEndian<std::uint16_t>(buffer, offset, 59);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(buffer, offset, 1);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(buffer, offset, 1);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(buffer, offset, 1);
  offset += sizeof(std::uint16_t);

  WriteLittleEndian<std::int64_t>(buffer, offset, 1'770'000'000'001'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(
      buffer, offset, static_cast<std::int8_t>(gate::types::Event::Update));
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 1'770'000'000'000'900);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 42);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(buffer, offset, -4);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int8_t>(buffer, offset, -3);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 650'125'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 17'500);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 650'120'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 21'000);
  offset += sizeof(std::int64_t);
  buffer[offset++] = 0;
  buffer[offset++] = 0;

  aquila::BookTicker book_ticker{};
  const bool decoded =
      DecodeBookTickerForTest(std::string_view{buffer.data(), offset},
                              1'770'000'000'001'200'000, 123, &book_ticker);

  ASSERT_TRUE(decoded);
  EXPECT_EQ(book_ticker.id, 42);
  EXPECT_EQ(book_ticker.symbol_id, 123);
  EXPECT_EQ(book_ticker.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(book_ticker.exchange_ns, 1'770'000'000'000'900'000);
  EXPECT_EQ(book_ticker.local_ns, 1'770'000'000'001'200'000);
  EXPECT_DOUBLE_EQ(book_ticker.bid_price, 65'012.0);
  EXPECT_DOUBLE_EQ(book_ticker.bid_volume, 21.0);
  EXPECT_DOUBLE_EQ(book_ticker.ask_price, 65'012.5);
  EXPECT_DOUBLE_EQ(book_ticker.ask_volume, 17.5);
}

TEST(GateSbeBookTickerDecoderTest, ExtractsBookTickerSymbolFromBboPayload) {
  std::array<char, 128> buffer{};
  size_t offset = 0;
  WriteLittleEndian<std::uint16_t>(buffer, offset, 59);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(buffer, offset, 1);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(buffer, offset, 1);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(buffer, offset, 1);
  offset += sizeof(std::uint16_t);

  WriteLittleEndian<std::int64_t>(buffer, offset, 1'770'000'000'001'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(
      buffer, offset, static_cast<std::int8_t>(gate::types::Event::Update));
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 1'770'000'000'000'900);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 42);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(buffer, offset, -4);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int8_t>(buffer, offset, -3);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 650'125'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 17'500);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 650'120'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(buffer, offset, 21'000);
  offset += sizeof(std::int64_t);
  WriteVarString(buffer, &offset, "futures.book_ticker");
  WriteVarString(buffer, &offset, "BTC_USDT");

  EXPECT_EQ(
      ExtractBookTickerSymbolForTest(std::string_view{buffer.data(), offset}),
      "BTC_USDT");
}
