#include "exchange/gate/sbe/message_dispatcher.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace {

template <typename T>
void WriteLittleEndian(std::array<char, 16>& buffer,
                       size_t offset,
                       T value) noexcept {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

std::string_view BuildHeader(std::array<char, 16>* buffer,
                             std::uint16_t block_length,
                             std::uint16_t template_id,
                             std::uint16_t schema_id,
                             std::uint16_t version) noexcept {
  WriteLittleEndian(*buffer, 0, block_length);
  WriteLittleEndian(*buffer, 2, template_id);
  WriteLittleEndian(*buffer, 4, schema_id);
  WriteLittleEndian(*buffer, 6, version);
  return {buffer->data(), aquila::gate::kSbeMessageHeaderBytes};
}

}  // namespace

TEST(GateSbeMessageDispatcherTest, ParsesMessageHeader) {
  std::array<char, 16> buffer{};
  const std::string_view payload =
      BuildHeader(&buffer, 59, 1, aquila::gate::kGateSbeSchemaId, 1);

  aquila::gate::SbeMessageHeader header{};

  ASSERT_TRUE(aquila::gate::ParseSbeMessageHeader(payload, &header));
  EXPECT_EQ(header.block_length, 59);
  EXPECT_EQ(header.template_id, 1);
  EXPECT_EQ(header.schema_id, aquila::gate::kGateSbeSchemaId);
  EXPECT_EQ(header.version, 1);
}

TEST(GateSbeMessageDispatcherTest, RequiresFullMessageHeader) {
  std::array<char, 16> buffer{};
  const std::string_view payload =
      BuildHeader(&buffer, 59, 1, aquila::gate::kGateSbeSchemaId, 1);
  aquila::gate::SbeMessageHeader header{};

  EXPECT_FALSE(aquila::gate::ParseSbeMessageHeader(payload.substr(0, 7),
                                                   &header));
}

TEST(GateSbeMessageDispatcherTest, SelectsGateSchemaAndBookTickerTemplate) {
  std::array<char, 16> buffer{};
  const std::string_view payload = BuildHeader(
      &buffer, 59, 1, aquila::gate::kGateSbeSchemaId,
      aquila::gate::kGateSbeSchemaVersion);

  const aquila::gate::SbeDispatchResult result =
      aquila::gate::DispatchSbeMessage(payload);

  EXPECT_EQ(result.status, aquila::gate::SbeDispatchStatus::kReady);
  EXPECT_EQ(result.message_type, aquila::gate::GateSbeMessageType::kBookTicker);
  EXPECT_EQ(result.header.template_id, 1);
}

TEST(GateSbeMessageDispatcherTest, SelectsKnownGateTemplates) {
  EXPECT_EQ(aquila::gate::GateSbeMessageTypeFromTemplateId(2),
            aquila::gate::GateSbeMessageType::kPublicTrade);
  EXPECT_EQ(aquila::gate::GateSbeMessageTypeFromTemplateId(3),
            aquila::gate::GateSbeMessageType::kObu);
  EXPECT_EQ(aquila::gate::GateSbeMessageTypeFromTemplateId(4),
            aquila::gate::GateSbeMessageType::kOrderBook);
  EXPECT_EQ(aquila::gate::GateSbeMessageTypeFromTemplateId(5),
            aquila::gate::GateSbeMessageType::kOrderBookUpdate);
  EXPECT_EQ(aquila::gate::GateSbeMessageTypeFromTemplateId(10),
            aquila::gate::GateSbeMessageType::kOrders);
}

TEST(GateSbeMessageDispatcherTest, RejectsUnsupportedSchemaBeforeTemplate) {
  std::array<char, 16> buffer{};
  const std::string_view payload = BuildHeader(
      &buffer, 59, 1, static_cast<std::uint16_t>(99),
      aquila::gate::kGateSbeSchemaVersion);

  const aquila::gate::SbeDispatchResult result =
      aquila::gate::DispatchSbeMessage(payload);

  EXPECT_EQ(result.status, aquila::gate::SbeDispatchStatus::kUnsupportedSchema);
  EXPECT_EQ(result.message_type, aquila::gate::GateSbeMessageType::kUnknown);
  EXPECT_EQ(result.header.schema_id, 99);
}

TEST(GateSbeMessageDispatcherTest, RejectsUnsupportedGateSchemaVersion) {
  std::array<char, 16> buffer{};
  const std::string_view payload = BuildHeader(
      &buffer, 59, 1, aquila::gate::kGateSbeSchemaId,
      static_cast<std::uint16_t>(99));

  const aquila::gate::SbeDispatchResult result =
      aquila::gate::DispatchSbeMessage(payload);

  EXPECT_EQ(result.status,
            aquila::gate::SbeDispatchStatus::kUnsupportedSchemaVersion);
  EXPECT_EQ(result.message_type, aquila::gate::GateSbeMessageType::kUnknown);
}

TEST(GateSbeMessageDispatcherTest, RejectsUnsupportedGateTemplate) {
  std::array<char, 16> buffer{};
  const std::string_view payload = BuildHeader(
      &buffer, 59, static_cast<std::uint16_t>(99),
      aquila::gate::kGateSbeSchemaId, aquila::gate::kGateSbeSchemaVersion);

  const aquila::gate::SbeDispatchResult result =
      aquila::gate::DispatchSbeMessage(payload);

  EXPECT_EQ(result.status,
            aquila::gate::SbeDispatchStatus::kUnsupportedTemplate);
  EXPECT_EQ(result.message_type, aquila::gate::GateSbeMessageType::kUnknown);
  EXPECT_EQ(result.header.template_id, 99);
}
