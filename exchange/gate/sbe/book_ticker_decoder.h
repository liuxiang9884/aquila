#ifndef AQUILA_EXCHANGE_GATE_SBE_BOOK_TICKER_DECODER_H_
#define AQUILA_EXCHANGE_GATE_SBE_BOOK_TICKER_DECODER_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <sbepp/sbepp.hpp>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "exchange/gate/sbe/generated/gate/messages/bbo.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"
#include "exchange/gate/sbe/message_dispatcher.h"

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

inline double DecimalExponentScale(std::int8_t exponent) noexcept {
  // Gate decimal fields are expected to stay within 10 decimal places. Keep
  // this table fixed; callers that need wider precision must handle it before
  // reaching this decoder.
  static constexpr double kNegativePowersOfTen[] = {
      1.0,      0.1,       0.01,       0.001,       0.0001,       0.00001,
      0.000001, 0.0000001, 0.00000001, 0.000000001, 0.0000000001,
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

template <typename BboView>
inline void AssignBookTickerFromView(const BboView& view, std::int64_t local_ns,
                                     std::int32_t symbol_id,
                                     BookTicker& out) noexcept {
  out.id = view.u().value();
  out.symbol_id = symbol_id;
  out.exchange = Exchange::kGate;
  out.exchange_ns = view.t().value() * 1000;
  out.local_ns = local_ns;

  const double price_scale = DecimalExponentScale(view.pxExponent().value());
  const double volume_scale = DecimalExponentScale(view.szExponent().value());
  out.bid_price =
      static_cast<double>(view.bidMantissaPrice().value()) * price_scale;
  out.bid_volume =
      static_cast<double>(view.bidMantissaSize().value()) * volume_scale;
  out.ask_price =
      static_cast<double>(view.askMantissaPrice().value()) * price_scale;
  out.ask_volume =
      static_cast<double>(view.askMantissaSize().value()) * volume_scale;
}

}  // namespace detail

// Conservative comparison helpers are kept out of this production header:
// DecimalMantissaToDoubleForTest, ExtractBookTickerSymbolForTest, and
// DecodeBookTickerForTest live in
// test/exchange/gate/sbe/book_ticker_decoder_test.cpp;
// BuildBookTickerPayload lives in
// evaluation/exchange/gate/sbe/book_ticker_payload_builder.h;
// DecodeBookTickerWithHeaderBenchmark lives in
// benchmark/exchange/gate/market_data/futures_market_data_benchmark.cpp.
inline std::string_view ExtractTrustedBookTickerSymbol(
    std::string_view payload, const SbeMessageHeader& header) noexcept {
  assert(detail::IsBookTickerHeader(header));
  assert(payload.size() >= detail::kMinBookTickerPayloadBytes);

  size_t offset = kSbeMessageHeaderBytes + detail::kBookTickerBlockLength;
  std::string_view channel;
  std::string_view symbol;
  [[maybe_unused]] bool ok = detail::ReadVarString8(payload, offset, channel);
  assert(ok);
  ok = detail::ReadVarString8(payload, offset, symbol);
  assert(ok);
  return symbol;
}

inline void DecodeTrustedBookTickerWithHeader(std::string_view payload,
                                              const SbeMessageHeader& header,
                                              std::int64_t local_ns,
                                              std::int32_t symbol_id,
                                              BookTicker& out) noexcept {
  assert(payload.size() >= detail::kMinBookTickerPayloadBytes);
  assert(detail::IsBookTickerHeader(header));

  const auto view = ::sbepp::make_const_view<::gate::messages::bbo>(
      payload.data(), payload.size());
  // Gate BBO binary market data frames are update events. Other SBE templates
  // must define their own event contract instead of reusing this assumption.
  assert(view.e() == ::gate::types::Event::Update);

  detail::AssignBookTickerFromView(view, local_ns, symbol_id, out);
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_SBE_BOOK_TICKER_DECODER_H_
