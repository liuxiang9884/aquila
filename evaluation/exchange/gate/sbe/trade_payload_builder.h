#ifndef AQUILA_EVALUATION_EXCHANGE_GATE_SBE_TRADE_PAYLOAD_BUILDER_H_
#define AQUILA_EVALUATION_EXCHANGE_GATE_SBE_TRADE_PAYLOAD_BUILDER_H_

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <sbepp/sbepp.hpp>

#include "evaluation/exchange/gate/sbe/book_ticker_payload_builder.h"
#include "exchange/gate/sbe/generated/gate/messages/publicTrade.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"
#include "exchange/gate/sbe/message_dispatcher.h"

namespace aquila::gate::evaluation {

struct PublicTradePayloadEntry {
  std::int64_t t;
  std::uint64_t id;
  std::int64_t size;
  std::int64_t price;
};

template <size_t N>
std::string_view BuildPublicTradePayload(
    std::array<char, N>* buffer, std::string_view symbol,
    std::span<const PublicTradePayloadEntry> entries,
    std::uint16_t template_id = kGateSbePublicTradeTemplateId,
    std::uint16_t block_length = ::sbepp::message_traits<
        ::gate::schema::messages::publicTrade>::block_length()) noexcept {
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
  WriteLittleEndian<std::int8_t>(*buffer, offset, -4);
  offset += sizeof(std::int8_t);
  WriteLittleEndian<std::int8_t>(*buffer, offset, -3);
  offset += sizeof(std::int8_t);

  WriteLittleEndian<std::uint16_t>(*buffer, offset, 32);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset,
                                   static_cast<std::uint16_t>(entries.size()));
  offset += sizeof(std::uint16_t);
  for (const PublicTradePayloadEntry& entry : entries) {
    WriteLittleEndian<std::int64_t>(*buffer, offset, entry.t);
    offset += sizeof(std::int64_t);
    WriteLittleEndian<std::uint64_t>(*buffer, offset, entry.id);
    offset += sizeof(std::uint64_t);
    WriteLittleEndian<std::int64_t>(*buffer, offset, entry.size);
    offset += sizeof(std::int64_t);
    WriteLittleEndian<std::int64_t>(*buffer, offset, entry.price);
    offset += sizeof(std::int64_t);
  }

  WriteVarString(*buffer, &offset, "futures.trades");
  WriteVarString(*buffer, &offset, symbol);
  return {buffer->data(), offset};
}

}  // namespace aquila::gate::evaluation

#endif  // AQUILA_EVALUATION_EXCHANGE_GATE_SBE_TRADE_PAYLOAD_BUILDER_H_
