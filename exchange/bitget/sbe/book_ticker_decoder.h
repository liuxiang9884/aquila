#ifndef AQUILA_EXCHANGE_BITGET_SBE_BOOK_TICKER_DECODER_H_
#define AQUILA_EXCHANGE_BITGET_SBE_BOOK_TICKER_DECODER_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "exchange/bitget/sbe/message_dispatcher.h"

namespace aquila::bitget {

namespace detail {

inline constexpr size_t kBookTickerTimestampOffset = kSbeMessageHeaderBytes;
inline constexpr size_t kBookTickerBidPriceOffset =
    kBookTickerTimestampOffset + sizeof(std::uint64_t);
inline constexpr size_t kBookTickerBidSizeOffset =
    kBookTickerBidPriceOffset + sizeof(std::int64_t);
inline constexpr size_t kBookTickerAskPriceOffset =
    kBookTickerBidSizeOffset + sizeof(std::int64_t);
inline constexpr size_t kBookTickerAskSizeOffset =
    kBookTickerAskPriceOffset + sizeof(std::int64_t);
inline constexpr size_t kBookTickerPriceExponentOffset =
    kBookTickerAskSizeOffset + sizeof(std::int64_t);
inline constexpr size_t kBookTickerSizeExponentOffset =
    kBookTickerPriceExponentOffset + sizeof(std::int8_t);
inline constexpr size_t kBookTickerSequenceOffset =
    kBookTickerSizeExponentOffset + sizeof(std::int8_t);
inline constexpr size_t kBookTickerStsOffset =
    kBookTickerSequenceOffset + sizeof(std::uint64_t);
inline constexpr size_t kBookTickerCategoryOffset =
    kBookTickerStsOffset + sizeof(std::uint64_t);
inline constexpr std::uint16_t kBookTickerBlockLengthWithoutSts = 50;
inline constexpr std::uint16_t kBookTickerBlockLengthWithSts = 58;
inline constexpr std::uint16_t kBookTickerBlockLengthWithStsCategory = 59;
inline constexpr std::uint16_t kLiveObservedBookTickerBlockLength = 64;

inline bool IsBookTickerHeader(const SbeMessageHeader& header) noexcept {
  return header.block_length >= kBookTickerBlockLengthWithoutSts &&
         header.template_id == kBitgetSbeBookTickerTemplateId &&
         header.schema_id == kBitgetSbeSchemaId &&
         header.version == kBitgetSbeSchemaVersion;
}

inline size_t MinBookTickerPayloadBytes(
    const SbeMessageHeader& header) noexcept {
  return kSbeMessageHeaderBytes + header.block_length + 1;
}

template <typename T>
inline T ReadLittleEndianUnchecked(std::string_view payload,
                                   size_t offset) noexcept {
  T value{};
  std::memcpy(&value, payload.data() + offset, sizeof(value));
  return value;
}

inline bool ReadVarString8(std::string_view payload, size_t& offset,
                           std::string_view& out) noexcept {
  if (offset >= payload.size()) {
    return false;
  }

  const size_t length = static_cast<unsigned char>(payload.data()[offset++]);
  if (payload.size() - offset < length) {
    return false;
  }

  out = payload.substr(offset, length);
  offset += length;
  return true;
}

inline bool TryExtractBookTickerSymbol(std::string_view payload,
                                       const SbeMessageHeader& header,
                                       std::string_view* out) noexcept {
  assert(out != nullptr);
  if (!IsBookTickerHeader(header) ||
      payload.size() < MinBookTickerPayloadBytes(header)) {
    return false;
  }

  size_t offset = kSbeMessageHeaderBytes + header.block_length;
  return ReadVarString8(payload, offset, *out);
}

inline bool HasCompleteBookTickerPayload(
    std::string_view payload, const SbeMessageHeader& header) noexcept {
  std::string_view symbol;
  return TryExtractBookTickerSymbol(payload, header, &symbol);
}

inline double DecimalExponentScale(std::int8_t exponent) noexcept {
  static constexpr double kNegativePowersOfTen[] = {
      1.0,       0.1,        0.01,        0.001,        0.0001,
      0.00001,   0.000001,   0.0000001,   0.00000001,   0.000000001,
      0.0000000001};
  static constexpr double kPositivePowersOfTen[] = {
      1.0,       10.0,       100.0,       1'000.0,      10'000.0,
      100'000.0, 1'000'000.0, 10'000'000.0, 100'000'000.0, 1'000'000'000.0,
      10'000'000'000.0};
  static constexpr size_t kPowersOfTenCount =
      sizeof(kNegativePowersOfTen) / sizeof(kNegativePowersOfTen[0]);
  static_assert(kPowersOfTenCount ==
                sizeof(kPositivePowersOfTen) / sizeof(kPositivePowersOfTen[0]));

  const int exponent_value = exponent;
  if (exponent_value <= 0) [[likely]] {
    const int scale = -exponent_value;
    assert(static_cast<size_t>(scale) < kPowersOfTenCount);
    return kNegativePowersOfTen[scale];
  }
  assert(static_cast<size_t>(exponent_value) < kPowersOfTenCount);
  return kPositivePowersOfTen[exponent_value];
}

}  // namespace detail

inline std::string_view ExtractTrustedBookTickerSymbol(
    std::string_view payload, const SbeMessageHeader& header) noexcept {
  assert(detail::IsBookTickerHeader(header));
  assert(detail::HasCompleteBookTickerPayload(payload, header));

  std::string_view symbol;
  [[maybe_unused]] const bool ok =
      detail::TryExtractBookTickerSymbol(payload, header, &symbol);
  assert(ok);
  return symbol;
}

// Caller must pass a payload/header pair already accepted by DispatchSbeMessage.
inline void DecodeBookTickerWithHeader(std::string_view payload,
                                       const SbeMessageHeader& header,
                                       std::int64_t local_ns,
                                       std::int32_t symbol_id,
                                       BookTicker& out) noexcept {
  assert(detail::IsBookTickerHeader(header));
  assert(payload.size() >= detail::MinBookTickerPayloadBytes(header));

  const std::uint64_t event_ms =
      detail::ReadLittleEndianUnchecked<std::uint64_t>(
          payload, detail::kBookTickerTimestampOffset);
  const std::int64_t bid_price =
      detail::ReadLittleEndianUnchecked<std::int64_t>(
          payload, detail::kBookTickerBidPriceOffset);
  const std::int64_t bid_size =
      detail::ReadLittleEndianUnchecked<std::int64_t>(
          payload, detail::kBookTickerBidSizeOffset);
  const std::int64_t ask_price =
      detail::ReadLittleEndianUnchecked<std::int64_t>(
          payload, detail::kBookTickerAskPriceOffset);
  const std::int64_t ask_size =
      detail::ReadLittleEndianUnchecked<std::int64_t>(
          payload, detail::kBookTickerAskSizeOffset);
  const std::int8_t price_exponent =
      detail::ReadLittleEndianUnchecked<std::int8_t>(
          payload, detail::kBookTickerPriceExponentOffset);
  const std::int8_t size_exponent =
      detail::ReadLittleEndianUnchecked<std::int8_t>(
          payload, detail::kBookTickerSizeExponentOffset);
  const std::uint64_t sequence =
      detail::ReadLittleEndianUnchecked<std::uint64_t>(
          payload, detail::kBookTickerSequenceOffset);

  std::uint64_t exchange_ms = event_ms;
  if (header.block_length >= detail::kBookTickerBlockLengthWithSts) {
    exchange_ms = detail::ReadLittleEndianUnchecked<std::uint64_t>(
        payload, detail::kBookTickerStsOffset);
  }

  const double price_scale = detail::DecimalExponentScale(price_exponent);
  const double size_scale = detail::DecimalExponentScale(size_exponent);

  out.id = static_cast<std::int64_t>(sequence);
  out.symbol_id = symbol_id;
  out.exchange = Exchange::kBitget;
  out.exchange_ns = static_cast<std::int64_t>(exchange_ms * 1000ULL);
  out.event_ns = static_cast<std::int64_t>(event_ms * 1000ULL);
  out.local_ns = local_ns;
  out.bid_price = static_cast<double>(bid_price) * price_scale;
  out.bid_volume = static_cast<double>(bid_size) * size_scale;
  out.ask_price = static_cast<double>(ask_price) * price_scale;
  out.ask_volume = static_cast<double>(ask_size) * size_scale;
}

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_SBE_BOOK_TICKER_DECODER_H_
