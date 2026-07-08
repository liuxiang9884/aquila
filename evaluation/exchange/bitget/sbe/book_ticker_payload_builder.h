#ifndef AQUILA_EVALUATION_EXCHANGE_BITGET_SBE_BOOK_TICKER_PAYLOAD_BUILDER_H_
#define AQUILA_EVALUATION_EXCHANGE_BITGET_SBE_BOOK_TICKER_PAYLOAD_BUILDER_H_

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "exchange/bitget/sbe/message_dispatcher.h"

namespace aquila::bitget::evaluation {

struct BookTickerPayloadOptions {
  std::uint16_t block_length{59};
  std::uint16_t template_id{kBitgetSbeBookTickerTemplateId};
  std::uint64_t ts{1'700'000'000'000'001};
  std::int64_t bid1_price{6'569'038};
  std::int64_t bid1_size{15'000};
  std::int64_t ask1_price{6'569'042};
  std::int64_t ask1_size{20'000};
  std::int8_t price_exponent{-2};
  std::int8_t size_exponent{-4};
  std::uint64_t seq{42};
  std::uint64_t sts{1'700'000'000'001'001};
  bool include_sts{true};
  std::uint8_t category{1};
  bool include_category{true};
};

template <typename T, size_t N>
void WriteLittleEndian(std::array<char, N>& buffer, size_t offset,
                       T value) noexcept {
  assert(offset + sizeof(value) <= buffer.size());
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

template <size_t N>
void WriteVarString8(std::array<char, N>& buffer, size_t* offset,
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
    BookTickerPayloadOptions options = {}) noexcept {
  assert(buffer != nullptr);

  size_t offset = 0;
  WriteLittleEndian<std::uint16_t>(*buffer, offset, options.block_length);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, options.template_id);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kBitgetSbeSchemaId);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kBitgetSbeSchemaVersion);
  offset += sizeof(std::uint16_t);

  WriteLittleEndian<std::uint64_t>(*buffer, offset, options.ts);
  offset += sizeof(std::uint64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, options.bid1_price);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, options.bid1_size);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, options.ask1_price);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int64_t>(*buffer, offset, options.ask1_size);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(*buffer, offset, options.price_exponent);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int8_t>(*buffer, offset, options.size_exponent);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::uint64_t>(*buffer, offset, options.seq);
  offset += sizeof(std::uint64_t);
  if (options.include_sts) {
    WriteLittleEndian<std::uint64_t>(*buffer, offset, options.sts);
    offset += sizeof(std::uint64_t);
  }
  if (options.include_category) {
    WriteLittleEndian<std::uint8_t>(*buffer, offset, options.category);
    offset += sizeof(std::uint8_t);
  }
  const size_t variable_offset = kSbeMessageHeaderBytes + options.block_length;
  assert(offset <= variable_offset);
  offset = variable_offset;
  WriteVarString8(*buffer, &offset, symbol);
  return {buffer->data(), offset};
}

}  // namespace aquila::bitget::evaluation

#endif  // AQUILA_EVALUATION_EXCHANGE_BITGET_SBE_BOOK_TICKER_PAYLOAD_BUILDER_H_
