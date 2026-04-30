#include "exchange/gate/sbe/book_ticker_decoder.h"

#include <array>
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

}  // namespace

TEST(GateSbeBookTickerDecoderTest, ConvertsDecimalMantissaWithinFixedTable) {
  EXPECT_DOUBLE_EQ(aquila::gate::detail::DecimalMantissaToDouble(123, 0),
                   123.0);
  EXPECT_DOUBLE_EQ(aquila::gate::detail::DecimalMantissaToDouble(123, 10),
                   1'230'000'000'000.0);
  EXPECT_DOUBLE_EQ(aquila::gate::detail::DecimalMantissaToDouble(123, -10),
                   0.0000000123);
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
  const bool decoded = aquila::gate::DecodeBookTicker(
      std::string_view{buffer.data(), offset}, 1'770'000'000'001'200'000, 123,
      &book_ticker);

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

  EXPECT_EQ(aquila::gate::ExtractBookTickerSymbol(
                std::string_view{buffer.data(), offset}),
            "BTC_USDT");
}
