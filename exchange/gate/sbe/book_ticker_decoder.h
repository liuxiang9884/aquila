#ifndef AQUILA_EXCHANGE_GATE_SBE_BOOK_TICKER_DECODER_H_
#define AQUILA_EXCHANGE_GATE_SBE_BOOK_TICKER_DECODER_H_

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "exchange/gate/sbe/generated/gate/messages/bbo.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"
#include "exchange/gate/sbe/message_dispatcher.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <sbepp/sbepp.hpp>

namespace aquila::gate {

namespace detail {

inline constexpr std::uint16_t kBookTickerBlockLength =
    ::sbepp::message_traits<::gate::schema::messages::bbo>::block_length();
inline constexpr size_t kMinBookTickerPayloadBytes =
    kSbeMessageHeaderBytes + kBookTickerBlockLength + 2;

inline bool IsBookTickerHeader(const SbeMessageHeader& header) noexcept {
  return header.block_length == kBookTickerBlockLength &&
         header.template_id == kGateSbeBookTickerTemplateId &&
         header.schema_id == kGateSbeSchemaId &&
         header.version == kGateSbeSchemaVersion;
}

inline bool ReadVarString8(std::string_view payload,
                           size_t* offset,
                           std::string_view* out) noexcept {
  if (offset == nullptr || out == nullptr || *offset >= payload.size()) {
    return false;
  }

  const size_t length =
      static_cast<unsigned char>(payload.data()[(*offset)++]);
  if (payload.size() - *offset < length) {
    return false;
  }

  *out = payload.substr(*offset, length);
  *offset += length;
  return true;
}

inline double DecimalExponentScale(std::int8_t exponent) noexcept {
  static constexpr double kNegativePowersOfTen[] = {
      1.0,
      0.1,
      0.01,
      0.001,
      0.0001,
      0.00001,
      0.000001,
      0.0000001,
      0.00000001,
      0.000000001,
      0.0000000001,
  };
  static constexpr double kPositivePowersOfTen[] = {
      1.0,
      10.0,
      100.0,
      1'000.0,
      10'000.0,
      100'000.0,
      1'000'000.0,
      10'000'000.0,
      100'000'000.0,
      1'000'000'000.0,
      10'000'000'000.0,
  };
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

inline double DecimalMantissaToDouble(std::int64_t mantissa,
                                      std::int8_t exponent) noexcept {
  return static_cast<double>(mantissa) * DecimalExponentScale(exponent);
}

}  // namespace detail

inline std::string_view ExtractBookTickerSymbol(
    std::string_view payload, const SbeMessageHeader& header) noexcept {
  if (payload.size() < detail::kMinBookTickerPayloadBytes ||
      !detail::IsBookTickerHeader(header)) {
    return {};
  }

  size_t offset = kSbeMessageHeaderBytes + detail::kBookTickerBlockLength;
  std::string_view channel;
  std::string_view symbol;
  if (!detail::ReadVarString8(payload, &offset, &channel) ||
      !detail::ReadVarString8(payload, &offset, &symbol)) {
    return {};
  }
  return symbol;
}

inline bool DecodeBookTickerWithHeader(std::string_view payload,
                                       const SbeMessageHeader& header,
                                       std::int64_t local_ns,
                                       std::int32_t symbol_id,
                                       BookTicker* out) noexcept {
  if (out == nullptr ||
      payload.size() < detail::kMinBookTickerPayloadBytes ||
      !detail::IsBookTickerHeader(header)) {
    return false;
  }

  const auto view =
      ::sbepp::make_const_view<::gate::messages::bbo>(payload.data(),
                                                      payload.size());
  if (view.e() != ::gate::types::Event::Update) {
    return false;
  }

  out->id = view.u().value();
  out->symbol_id = symbol_id;
  out->exchange = Exchange::kGate;
  out->exchange_ns = view.t().value() * 1000;
  out->local_ns = local_ns;

  const double price_scale =
      detail::DecimalExponentScale(view.pxExponent().value());
  const double volume_scale =
      detail::DecimalExponentScale(view.szExponent().value());
  out->bid_price =
      static_cast<double>(view.bidMantissaPrice().value()) * price_scale;
  out->bid_volume =
      static_cast<double>(view.bidMantissaSize().value()) * volume_scale;
  out->ask_price =
      static_cast<double>(view.askMantissaPrice().value()) * price_scale;
  out->ask_volume =
      static_cast<double>(view.askMantissaSize().value()) * volume_scale;
  return true;
}

inline bool DecodeBookTicker(std::string_view payload,
                             std::int64_t local_ns,
                             std::int32_t symbol_id,
                             BookTicker* out) noexcept {
  const SbeDispatchResult dispatch = DispatchSbeMessage(payload);
  if (dispatch.status != SbeDispatchStatus::kReady ||
      dispatch.message_type != GateSbeMessageType::kBookTicker) {
    return false;
  }

  return DecodeBookTickerWithHeader(payload, dispatch.header, local_ns,
                                    symbol_id, out);
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_SBE_BOOK_TICKER_DECODER_H_
