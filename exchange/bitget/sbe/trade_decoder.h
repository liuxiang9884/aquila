#ifndef AQUILA_EXCHANGE_BITGET_SBE_TRADE_DECODER_H_
#define AQUILA_EXCHANGE_BITGET_SBE_TRADE_DECODER_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "exchange/bitget/sbe/book_ticker_decoder.h"
#include "exchange/bitget/sbe/message_dispatcher.h"

namespace aquila::bitget {

namespace detail {

inline constexpr size_t kPublicTradePriceExponentOffset =
    kSbeMessageHeaderBytes;
inline constexpr size_t kPublicTradeSizeExponentOffset =
    kPublicTradePriceExponentOffset + sizeof(std::int8_t);
inline constexpr size_t kPublicTradeRootStsOffset =
    kPublicTradeSizeExponentOffset + sizeof(std::int8_t);
inline constexpr std::uint16_t kMinPublicTradeRootBlockLength = 2;
inline constexpr std::uint16_t kPublicTradeRootBlockLengthWithoutSts = 8;
inline constexpr std::uint16_t kLiveObservedPublicTradeRootBlockLength = 16;

inline constexpr size_t kSbeGroupHeaderBytes = 4;
inline constexpr size_t kPublicTradeEntryTsOffset = 0;
inline constexpr size_t kPublicTradeEntryExecIdOffset =
    kPublicTradeEntryTsOffset + sizeof(std::uint64_t);
inline constexpr size_t kPublicTradeEntryPriceOffset =
    kPublicTradeEntryExecIdOffset + sizeof(std::uint64_t);
inline constexpr size_t kPublicTradeEntrySizeOffset =
    kPublicTradeEntryPriceOffset + sizeof(std::int64_t);
inline constexpr size_t kPublicTradeEntrySideOffset =
    kPublicTradeEntrySizeOffset + sizeof(std::int64_t);
inline constexpr size_t kPublicTradeEntryStsOffset =
    kPublicTradeEntrySideOffset + sizeof(std::uint8_t);
inline constexpr std::uint16_t kPublicTradeEntryBlockLengthWithoutSts = 40;
inline constexpr std::uint16_t kPublicTradeEntryBlockLengthWithStsCategory = 42;

struct PublicTradeGroupHeader {
  std::uint16_t entry_block_length{0};
  std::uint16_t num_in_group{0};
};

inline bool IsPublicTradeHeader(const SbeMessageHeader& header) noexcept {
  return header.block_length >= kMinPublicTradeRootBlockLength &&
         header.template_id == kBitgetSbePublicTradeTemplateId &&
         header.schema_id == kBitgetSbeSchemaId &&
         IsSupportedBitgetSbeSchemaVersion(header.version);
}

inline size_t PublicTradeGroupOffset(const SbeMessageHeader& header) noexcept {
  return kSbeMessageHeaderBytes + header.block_length;
}

inline bool TryReadPublicTradeGroupHeader(
    std::string_view payload, const SbeMessageHeader& header,
    PublicTradeGroupHeader* out) noexcept {
  assert(out != nullptr);
  if (!IsPublicTradeHeader(header)) {
    return false;
  }

  const size_t group_offset = PublicTradeGroupOffset(header);
  if (payload.size() < group_offset + kSbeGroupHeaderBytes) {
    return false;
  }

  const std::uint16_t entry_block_length =
      ReadLittleEndianUnchecked<std::uint16_t>(payload, group_offset);
  if (entry_block_length < kPublicTradeEntryBlockLengthWithoutSts) {
    return false;
  }

  const std::uint16_t num_in_group = ReadLittleEndianUnchecked<std::uint16_t>(
      payload, group_offset + sizeof(std::uint16_t));
  const size_t entries_offset = group_offset + kSbeGroupHeaderBytes;
  if (entry_block_length != 0 &&
      static_cast<size_t>(num_in_group) >
          (payload.size() - entries_offset) / entry_block_length) {
    return false;
  }

  *out = {.entry_block_length = entry_block_length,
          .num_in_group = num_in_group};
  return true;
}

inline size_t PublicTradeSymbolOffset(
    const SbeMessageHeader& header,
    const PublicTradeGroupHeader& group_header) noexcept {
  return PublicTradeGroupOffset(header) + kSbeGroupHeaderBytes +
         static_cast<size_t>(group_header.entry_block_length) *
             group_header.num_in_group;
}

inline bool TryExtractTradeSymbol(std::string_view payload,
                                  const SbeMessageHeader& header,
                                  std::string_view* out) noexcept {
  assert(out != nullptr);
  PublicTradeGroupHeader group_header{};
  if (!TryReadPublicTradeGroupHeader(payload, header, &group_header)) {
    return false;
  }

  size_t offset = PublicTradeSymbolOffset(header, group_header);
  return ReadVarString8(payload, offset, *out);
}

inline bool HasCompleteTradePayload(std::string_view payload,
                                    const SbeMessageHeader& header) noexcept {
  std::string_view symbol;
  return TryExtractTradeSymbol(payload, header, &symbol);
}

inline bool HasRootTradeSts(const SbeMessageHeader& header) noexcept {
  return header.block_length >= kPublicTradeRootStsOffset -
                                    kSbeMessageHeaderBytes +
                                    sizeof(std::uint64_t);
}

inline bool HasEntryTradeSts(
    const PublicTradeGroupHeader& group_header) noexcept {
  return group_header.entry_block_length >=
         kPublicTradeEntryBlockLengthWithStsCategory;
}

inline void AssignTradeFromPayload(std::string_view payload,
                                   size_t entry_offset,
                                   std::uint64_t fallback_exchange_us,
                                   std::int64_t local_ns,
                                   std::int32_t symbol_id, double price_scale,
                                   double size_scale, std::uint32_t batch_index,
                                   std::uint32_t batch_count,
                                   bool has_entry_sts, Trade& out) noexcept {
  const std::uint64_t event_us = ReadLittleEndianUnchecked<std::uint64_t>(
      payload, entry_offset + kPublicTradeEntryTsOffset);
  const std::uint64_t exec_id = ReadLittleEndianUnchecked<std::uint64_t>(
      payload, entry_offset + kPublicTradeEntryExecIdOffset);
  const std::int64_t price = ReadLittleEndianUnchecked<std::int64_t>(
      payload, entry_offset + kPublicTradeEntryPriceOffset);
  const std::int64_t size = ReadLittleEndianUnchecked<std::int64_t>(
      payload, entry_offset + kPublicTradeEntrySizeOffset);
  const std::uint8_t side = ReadLittleEndianUnchecked<std::uint8_t>(
      payload, entry_offset + kPublicTradeEntrySideOffset);

  std::uint64_t exchange_us = fallback_exchange_us;
  if (has_entry_sts) {
    const std::uint64_t entry_sts = ReadLittleEndianUnchecked<std::uint64_t>(
        payload, entry_offset + kPublicTradeEntryStsOffset);
    if (entry_sts != 0) {
      exchange_us = entry_sts;
    }
  }
  if (exchange_us == 0) {
    exchange_us = event_us;
  }

  assert(side <= 1);
  out.id = static_cast<std::int64_t>(exec_id);
  out.symbol_id = symbol_id;
  out.exchange = Exchange::kBitget;
  out.side = side == 0 ? OrderSide::kBuy : OrderSide::kSell;
  out.reserved = 0;
  out.exchange_ns = static_cast<std::int64_t>(exchange_us * 1000ULL);
  out.event_ns = static_cast<std::int64_t>(event_us * 1000ULL);
  out.local_ns = local_ns;
  out.price = static_cast<double>(price) * price_scale;
  out.volume = static_cast<double>(size) * size_scale;
  out.batch_index = batch_index;
  out.batch_count = batch_count;
}

}  // namespace detail

inline std::string_view ExtractTrustedTradeSymbol(
    std::string_view payload, const SbeMessageHeader& header) noexcept {
  assert(detail::IsPublicTradeHeader(header));
  assert(detail::HasCompleteTradePayload(payload, header));

  std::string_view symbol;
  [[maybe_unused]] const bool ok =
      detail::TryExtractTradeSymbol(payload, header, &symbol);
  assert(ok);
  return symbol;
}

template <typename Handler>
inline void DecodeTradesWithHeader(std::string_view payload,
                                   const SbeMessageHeader& header,
                                   std::int64_t local_ns,
                                   std::int32_t symbol_id,
                                   Handler&& handler) noexcept {
  DecodeTradesWithHeaderToSlots(payload, header, local_ns, symbol_id,
                                [&](auto&& fill_slot) noexcept {
                                  Trade trade{};
                                  fill_slot(trade);
                                  std::forward<Handler>(handler)(trade);
                                });
}

template <typename WriterFactory>
inline void DecodeTradesWithHeaderToSlots(
    std::string_view payload, const SbeMessageHeader& header,
    std::int64_t local_ns, std::int32_t symbol_id,
    WriterFactory&& writer_factory) noexcept {
  assert(detail::IsPublicTradeHeader(header));
  assert(detail::HasCompleteTradePayload(payload, header));

  detail::PublicTradeGroupHeader group_header{};
  [[maybe_unused]] const bool ok =
      detail::TryReadPublicTradeGroupHeader(payload, header, &group_header);
  assert(ok);

  const std::int8_t price_exponent =
      detail::ReadLittleEndianUnchecked<std::int8_t>(
          payload, detail::kPublicTradePriceExponentOffset);
  const std::int8_t size_exponent =
      detail::ReadLittleEndianUnchecked<std::int8_t>(
          payload, detail::kPublicTradeSizeExponentOffset);
  const double price_scale = detail::DecimalExponentScale(price_exponent);
  const double size_scale = detail::DecimalExponentScale(size_exponent);

  std::uint64_t fallback_exchange_us = 0;
  if (detail::HasRootTradeSts(header)) {
    fallback_exchange_us = detail::ReadLittleEndianUnchecked<std::uint64_t>(
        payload, detail::kPublicTradeRootStsOffset);
  }

  size_t entry_offset =
      detail::PublicTradeGroupOffset(header) + detail::kSbeGroupHeaderBytes;
  const bool has_entry_sts = detail::HasEntryTradeSts(group_header);
  const auto batch_count =
      static_cast<std::uint32_t>(group_header.num_in_group);
  for (std::uint32_t batch_index = 0; batch_index < batch_count;
       ++batch_index) {
    std::forward<WriterFactory>(writer_factory)([&](Trade& out) noexcept {
      detail::AssignTradeFromPayload(payload, entry_offset,
                                     fallback_exchange_us, local_ns, symbol_id,
                                     price_scale, size_scale, batch_index,
                                     batch_count, has_entry_sts, out);
    });
    entry_offset += group_header.entry_block_length;
  }
}

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_SBE_TRADE_DECODER_H_
