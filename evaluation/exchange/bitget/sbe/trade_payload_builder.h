#ifndef AQUILA_EVALUATION_EXCHANGE_BITGET_SBE_TRADE_PAYLOAD_BUILDER_H_
#define AQUILA_EVALUATION_EXCHANGE_BITGET_SBE_TRADE_PAYLOAD_BUILDER_H_

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "evaluation/exchange/bitget/sbe/book_ticker_payload_builder.h"
#include "exchange/bitget/sbe/message_dispatcher.h"

namespace aquila::bitget::evaluation {

struct PublicTradePayloadEntry {
  std::uint64_t ts{1'700'000'000'000'002};
  std::uint64_t exec_id{9999};
  std::int64_t price{6'566'738};
  std::int64_t size{5'000};
  std::uint8_t side{0};
  std::uint64_t sts{1'700'000'000'001'002};
  std::uint8_t category{1};
};

struct PublicTradePayloadOptions {
  std::uint16_t block_length{16};
  std::uint16_t template_id{kBitgetSbePublicTradeTemplateId};
  std::uint16_t schema_version{kBitgetSbeLiveSchemaVersion};
  std::int8_t price_exponent{-2};
  std::int8_t size_exponent{-4};
  std::uint64_t sts{1'700'000'000'001'002};
  std::uint8_t category{1};
  std::uint16_t entry_block_length{40};
};

template <size_t N>
std::string_view BuildPublicTradePayload(
    std::array<char, N>* buffer, std::string_view symbol,
    std::span<const PublicTradePayloadEntry> entries,
    PublicTradePayloadOptions options = {}) noexcept {
  assert(buffer != nullptr);
  assert(options.block_length >= 2);
  assert(options.entry_block_length >= 40);

  size_t offset = 0;
  WriteLittleEndian<std::uint16_t>(*buffer, offset, options.block_length);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, options.template_id);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kBitgetSbeSchemaId);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, options.schema_version);
  offset += sizeof(std::uint16_t);

  const size_t root_offset = offset;
  WriteLittleEndian<std::int8_t>(*buffer, offset, options.price_exponent);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int8_t>(*buffer, offset, options.size_exponent);
  offset += sizeof(std::int8_t);
  if (options.block_length >= 10) {
    WriteLittleEndian<std::uint64_t>(*buffer, offset, options.sts);
    offset += sizeof(std::uint64_t);
  }
  if (options.block_length >= 11) {
    WriteLittleEndian<std::uint8_t>(*buffer, offset, options.category);
    offset += sizeof(std::uint8_t);
  }
  offset = root_offset + options.block_length;

  WriteLittleEndian<std::uint16_t>(*buffer, offset, options.entry_block_length);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset,
                                   static_cast<std::uint16_t>(entries.size()));
  offset += sizeof(std::uint16_t);

  for (const PublicTradePayloadEntry& entry : entries) {
    const size_t entry_offset = offset;
    WriteLittleEndian<std::uint64_t>(*buffer, offset, entry.ts);
    offset += sizeof(std::uint64_t);
    WriteLittleEndian<std::uint64_t>(*buffer, offset, entry.exec_id);
    offset += sizeof(std::uint64_t);
    WriteLittleEndian<std::int64_t>(*buffer, offset, entry.price);
    offset += sizeof(std::int64_t);
    WriteLittleEndian<std::int64_t>(*buffer, offset, entry.size);
    offset += sizeof(std::int64_t);
    WriteLittleEndian<std::uint8_t>(*buffer, offset, entry.side);
    offset += sizeof(std::uint8_t);
    if (options.entry_block_length >= 42) {
      WriteLittleEndian<std::uint64_t>(*buffer, offset, entry.sts);
      offset += sizeof(std::uint64_t);
      WriteLittleEndian<std::uint8_t>(*buffer, offset, entry.category);
      offset += sizeof(std::uint8_t);
    }
    offset = entry_offset + options.entry_block_length;
  }

  WriteVarString8(*buffer, &offset, symbol);
  return {buffer->data(), offset};
}

}  // namespace aquila::bitget::evaluation

#endif  // AQUILA_EVALUATION_EXCHANGE_BITGET_SBE_TRADE_PAYLOAD_BUILDER_H_
