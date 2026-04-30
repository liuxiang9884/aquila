#ifndef AQUILA_EXCHANGE_GATE_SBE_TEST_SUPPORT_BOOK_TICKER_PAYLOAD_BUILDER_H_
#define AQUILA_EXCHANGE_GATE_SBE_TEST_SUPPORT_BOOK_TICKER_PAYLOAD_BUILDER_H_

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <sbepp/sbepp.hpp>

#include "exchange/gate/sbe/generated/gate/messages/bbo.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"
#include "exchange/gate/sbe/message_dispatcher.h"

namespace aquila::gate::test_support {

inline constexpr std::uint16_t kBookTickerBlockLength =
    ::sbepp::message_traits<::gate::schema::messages::bbo>::block_length();

template <typename T, size_t N>
void WriteLittleEndian(std::array<char, N>& buffer, size_t offset,
                       T value) noexcept {
  assert(offset + sizeof(value) <= buffer.size());
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

template <size_t N>
void WriteVarString(std::array<char, N>& buffer, size_t* offset,
                    std::string_view value) noexcept {
  assert(offset != nullptr);
  assert(value.size() <= 255);
  assert(*offset + 1 + value.size() <= buffer.size());
  buffer[(*offset)++] = static_cast<char>(value.size());
  std::memcpy(buffer.data() + *offset, value.data(), value.size());
  *offset += value.size();
}

template <size_t N>
std::string_view BuildBookTickerPayload(
    std::array<char, N>* buffer, std::string_view symbol,
    std::uint16_t template_id = kGateSbeBookTickerTemplateId,
    std::uint16_t block_length = kBookTickerBlockLength) noexcept {
  assert(buffer != nullptr);

  size_t offset = 0;
  WriteLittleEndian<std::uint16_t>(*buffer, offset, block_length);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, template_id);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kGateSbeSchemaId);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kGateSbeSchemaVersion);
  offset += sizeof(std::uint16_t);

  WriteLittleEndian<std::int64_t>(*buffer, offset, 1'770'000'000'001'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(
      *buffer, offset, static_cast<std::int8_t>(::gate::types::Event::Update));
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 1'770'000'000'000'900);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 42);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(*buffer, offset, -4);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int8_t>(*buffer, offset, -3);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 650'125'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 17'500);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 650'120'000);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, 21'000);
  offset += sizeof(std::int64_t);
  WriteVarString(*buffer, &offset, "futures.book_ticker");
  WriteVarString(*buffer, &offset, symbol);
  return {buffer->data(), offset};
}

}  // namespace aquila::gate::test_support

#endif  // AQUILA_EXCHANGE_GATE_SBE_TEST_SUPPORT_BOOK_TICKER_PAYLOAD_BUILDER_H_
