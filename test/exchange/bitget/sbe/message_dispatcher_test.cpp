#include "exchange/bitget/sbe/message_dispatcher.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

namespace {

template <typename T>
void WriteLittleEndian(std::array<char, 16>& buffer, size_t offset,
                       T value) noexcept {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

std::string_view BuildHeader(std::array<char, 16>* buffer,
                             std::uint16_t block_length,
                             std::uint16_t template_id, std::uint16_t schema_id,
                             std::uint16_t version) noexcept {
  WriteLittleEndian(*buffer, 0, block_length);
  WriteLittleEndian(*buffer, 2, template_id);
  WriteLittleEndian(*buffer, 4, schema_id);
  WriteLittleEndian(*buffer, 6, version);
  return {buffer->data(), aquila::bitget::kSbeMessageHeaderBytes};
}

}  // namespace

TEST(BitgetSbeMessageDispatcherTest, ParsesMessageHeader) {
  std::array<char, 16> buffer{};
  const std::string_view payload =
      BuildHeader(&buffer, 59, 1002, aquila::bitget::kBitgetSbeSchemaId,
                  aquila::bitget::kBitgetSbeSchemaVersion);

  aquila::bitget::SbeMessageHeader header{};

  ASSERT_TRUE(aquila::bitget::ParseSbeMessageHeader(payload, &header));
  EXPECT_EQ(header.block_length, 59);
  EXPECT_EQ(header.template_id, 1002);
  EXPECT_EQ(header.schema_id, aquila::bitget::kBitgetSbeSchemaId);
  EXPECT_EQ(header.version, aquila::bitget::kBitgetSbeSchemaVersion);
}

TEST(BitgetSbeMessageDispatcherTest, RequiresFullMessageHeader) {
  std::array<char, 16> buffer{};
  const std::string_view payload =
      BuildHeader(&buffer, 59, 1002, aquila::bitget::kBitgetSbeSchemaId,
                  aquila::bitget::kBitgetSbeSchemaVersion);
  aquila::bitget::SbeMessageHeader header{};

  EXPECT_FALSE(
      aquila::bitget::ParseSbeMessageHeader(payload.substr(0, 7), &header));
}

TEST(BitgetSbeMessageDispatcherTest, DispatchesBooks1Template) {
  const aquila::bitget::SbeMessageHeader header{
      .block_length = 59,
      .template_id = 1002,
      .schema_id = 1,
      .version = 2,
  };

  const aquila::bitget::SbeDispatchResult result =
      aquila::bitget::DispatchBitgetSbeMessage(header);

  EXPECT_EQ(result.status, aquila::bitget::SbeDispatchStatus::kReady);
  EXPECT_EQ(result.message_type,
            aquila::bitget::BitgetSbeMessageType::kBookTicker);
}

TEST(BitgetSbeMessageDispatcherTest, DispatchesLiveSchemaVersion3Books1) {
  const aquila::bitget::SbeMessageHeader header{
      .block_length = 64,
      .template_id = aquila::bitget::kBitgetSbeBookTickerTemplateId,
      .schema_id = aquila::bitget::kBitgetSbeSchemaId,
      .version = aquila::bitget::kBitgetSbeLiveSchemaVersion,
  };

  const aquila::bitget::SbeDispatchResult result =
      aquila::bitget::DispatchBitgetSbeMessage(header);

  EXPECT_EQ(result.status, aquila::bitget::SbeDispatchStatus::kReady);
  EXPECT_EQ(result.message_type,
            aquila::bitget::BitgetSbeMessageType::kBookTicker);
}

TEST(BitgetSbeMessageDispatcherTest, DispatchesBooks1Payload) {
  std::array<char, 16> buffer{};
  const std::string_view payload =
      BuildHeader(&buffer, 59, aquila::bitget::kBitgetSbeBookTickerTemplateId,
                  aquila::bitget::kBitgetSbeSchemaId,
                  aquila::bitget::kBitgetSbeSchemaVersion);

  const aquila::bitget::SbeDispatchResult result =
      aquila::bitget::DispatchSbeMessage(payload);

  EXPECT_EQ(result.status, aquila::bitget::SbeDispatchStatus::kReady);
  EXPECT_EQ(result.message_type,
            aquila::bitget::BitgetSbeMessageType::kBookTicker);
  EXPECT_EQ(result.header.template_id,
            aquila::bitget::kBitgetSbeBookTickerTemplateId);
}

TEST(BitgetSbeMessageDispatcherTest, RejectsUnknownTemplate) {
  const aquila::bitget::SbeMessageHeader header{
      .block_length = 59,
      .template_id = 9999,
      .schema_id = 1,
      .version = 2,
  };

  const aquila::bitget::SbeDispatchResult result =
      aquila::bitget::DispatchBitgetSbeMessage(header);

  EXPECT_EQ(result.status,
            aquila::bitget::SbeDispatchStatus::kUnsupportedTemplate);
  EXPECT_EQ(result.message_type,
            aquila::bitget::BitgetSbeMessageType::kUnknown);
}

TEST(BitgetSbeMessageDispatcherTest, RejectsUnsupportedSchemaBeforeTemplate) {
  const aquila::bitget::SbeMessageHeader header{
      .block_length = 59,
      .template_id = aquila::bitget::kBitgetSbeBookTickerTemplateId,
      .schema_id = 99,
      .version = aquila::bitget::kBitgetSbeSchemaVersion,
  };

  const aquila::bitget::SbeDispatchResult result =
      aquila::bitget::DispatchBitgetSbeMessage(header);

  EXPECT_EQ(result.status,
            aquila::bitget::SbeDispatchStatus::kUnsupportedSchema);
  EXPECT_EQ(result.message_type,
            aquila::bitget::BitgetSbeMessageType::kUnknown);
}

TEST(BitgetSbeMessageDispatcherTest, RejectsUnsupportedSchemaVersion) {
  const aquila::bitget::SbeMessageHeader header{
      .block_length = 59,
      .template_id = aquila::bitget::kBitgetSbeBookTickerTemplateId,
      .schema_id = aquila::bitget::kBitgetSbeSchemaId,
      .version = 99,
  };

  const aquila::bitget::SbeDispatchResult result =
      aquila::bitget::DispatchBitgetSbeMessage(header);

  EXPECT_EQ(result.status,
            aquila::bitget::SbeDispatchStatus::kUnsupportedSchemaVersion);
  EXPECT_EQ(result.message_type,
            aquila::bitget::BitgetSbeMessageType::kUnknown);
}
