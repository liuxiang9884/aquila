#include "exchange/gate/sbe/generated/bbo.h"
#include "exchange/gate/sbe/generated/Event.h"
#include "exchange/gate/sbe/generated/MessageHeader.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

template <typename T>
void WriteLittleEndian(std::array<char, 128>& buffer,
                       size_t offset,
                       T value) noexcept {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

}  // namespace

TEST(GateSbeGeneratedTest, DecodesBboFixedFields) {
  using gate::sbe::Event;
  using gate::sbe::MessageHeader;
  using gate::sbe::bbo;

  std::array<char, 128> buffer{};
  constexpr size_t message_offset = 5;
  size_t offset = message_offset;
  WriteLittleEndian<std::uint16_t>(buffer, offset, bbo::sbeBlockLength());
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(buffer, offset, bbo::sbeTemplateId());
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(buffer, offset, bbo::sbeSchemaId());
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(buffer, offset, bbo::sbeSchemaVersion());
  offset += sizeof(std::uint16_t);

  WriteLittleEndian<std::int64_t>(buffer, offset, 1'770'000'000'001'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(buffer, offset,
                                 static_cast<std::int8_t>(Event::Update));
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

  MessageHeader header;
  header.wrap(buffer.data(), message_offset, 0, buffer.size());
  ASSERT_EQ(header.blockLength(), bbo::sbeBlockLength());
  ASSERT_EQ(header.templateId(), bbo::sbeTemplateId());
  ASSERT_EQ(header.schemaId(), bbo::sbeSchemaId());
  ASSERT_EQ(header.version(), bbo::sbeSchemaVersion());

  bbo decoded_bbo;
  decoded_bbo.wrapForDecode(
      buffer.data(), message_offset + MessageHeader::encodedLength(),
      header.blockLength(), header.version(), buffer.size());

  EXPECT_EQ(decoded_bbo.time(), 1'770'000'000'001'000);
  EXPECT_EQ(decoded_bbo.e(), Event::Update);
  EXPECT_EQ(decoded_bbo.t(), 1'770'000'000'000'900);
  EXPECT_EQ(decoded_bbo.u(), 42);
  EXPECT_EQ(decoded_bbo.pxExponent(), -4);
  EXPECT_EQ(decoded_bbo.szExponent(), -3);
  EXPECT_EQ(decoded_bbo.askMantissaPrice(), 650'125'000);
  EXPECT_EQ(decoded_bbo.askMantissaSize(), 17'500);
  EXPECT_EQ(decoded_bbo.bidMantissaPrice(), 650'120'000);
  EXPECT_EQ(decoded_bbo.bidMantissaSize(), 21'000);
}
