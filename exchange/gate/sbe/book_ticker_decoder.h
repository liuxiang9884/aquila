#ifndef AQUILA_EXCHANGE_GATE_SBE_BOOK_TICKER_DECODER_H_
#define AQUILA_EXCHANGE_GATE_SBE_BOOK_TICKER_DECODER_H_

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "exchange/gate/sbe/generated/gate/messages/bbo.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <sbepp/sbepp.hpp>

namespace aquila::gate {

namespace detail {

inline constexpr std::uint16_t kBookTickerBlockLength =
    ::sbepp::message_traits<::gate::schema::messages::bbo>::block_length();
inline constexpr std::uint16_t kBookTickerTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::bbo>::id();
inline constexpr std::uint16_t kBookTickerSchemaId =
    ::sbepp::schema_traits<::gate::schema>::id();
inline constexpr std::uint16_t kBookTickerSchemaVersion =
    ::sbepp::schema_traits<::gate::schema>::version();
inline constexpr size_t kSbeMessageHeaderBytes = 8;
inline constexpr size_t kMinBookTickerPayloadBytes =
    kSbeMessageHeaderBytes + kBookTickerBlockLength + 2;

inline std::uint16_t ReadUint16LittleEndian(std::string_view payload,
                                            size_t offset) noexcept {
  if (offset + sizeof(std::uint16_t) > payload.size()) {
    return 0;
  }
  const auto* bytes =
      reinterpret_cast<const unsigned char*>(payload.data() + offset);
  return static_cast<std::uint16_t>(bytes[0]) |
         static_cast<std::uint16_t>(bytes[1] << 8U);
}

inline double ApplyDecimalExponent(std::int64_t mantissa,
                                   std::int8_t exponent) noexcept {
  static constexpr double kPowersOfTen[] = {
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
      100'000'000'000.0,
      1'000'000'000'000.0,
      10'000'000'000'000.0,
      100'000'000'000'000.0,
      1'000'000'000'000'000.0,
      10'000'000'000'000'000.0,
      100'000'000'000'000'000.0,
      1'000'000'000'000'000'000.0,
  };
  static constexpr size_t kPowersOfTenCount =
      sizeof(kPowersOfTen) / sizeof(kPowersOfTen[0]);

  const double value = static_cast<double>(mantissa);
  const int exponent_value = exponent;
  if (exponent_value >= 0) {
    if (static_cast<size_t>(exponent_value) < kPowersOfTenCount) {
      return value * kPowersOfTen[exponent_value];
    }
    double multiplier = kPowersOfTen[kPowersOfTenCount - 1];
    for (int i = static_cast<int>(kPowersOfTenCount); i <= exponent_value; ++i) {
      multiplier *= 10.0;
    }
    return value * multiplier;
  }

  const int scale = -exponent_value;
  if (static_cast<size_t>(scale) < kPowersOfTenCount) {
    return value / kPowersOfTen[scale];
  }
  double divisor = kPowersOfTen[kPowersOfTenCount - 1];
  for (int i = static_cast<int>(kPowersOfTenCount); i <= scale; ++i) {
    divisor *= 10.0;
  }
  return value / divisor;
}

}  // namespace detail

inline bool DecodeBookTicker(std::string_view payload,
                             std::int64_t local_ns,
                             std::int32_t symbol_id,
                             BookTicker* out) noexcept {
  if (out == nullptr ||
      payload.size() < detail::kMinBookTickerPayloadBytes) {
    return false;
  }

  if (detail::ReadUint16LittleEndian(payload, 0) !=
          detail::kBookTickerBlockLength ||
      detail::ReadUint16LittleEndian(payload, 2) !=
          detail::kBookTickerTemplateId ||
      detail::ReadUint16LittleEndian(payload, 4) !=
          detail::kBookTickerSchemaId ||
      detail::ReadUint16LittleEndian(payload, 6) !=
          detail::kBookTickerSchemaVersion) {
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
  out->bid_price = detail::ApplyDecimalExponent(view.bidMantissaPrice().value(),
                                                view.pxExponent().value());
  out->bid_volume = detail::ApplyDecimalExponent(view.bidMantissaSize().value(),
                                                 view.szExponent().value());
  out->ask_price = detail::ApplyDecimalExponent(view.askMantissaPrice().value(),
                                                view.pxExponent().value());
  out->ask_volume = detail::ApplyDecimalExponent(view.askMantissaSize().value(),
                                                 view.szExponent().value());
  return true;
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_SBE_BOOK_TICKER_DECODER_H_
