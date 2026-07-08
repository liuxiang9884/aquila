#ifndef AQUILA_EXCHANGE_GATE_SBE_TRADE_DECODER_H_
#define AQUILA_EXCHANGE_GATE_SBE_TRADE_DECODER_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>

#include <sbepp/sbepp.hpp>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "exchange/gate/sbe/book_ticker_decoder.h"
#include "exchange/gate/sbe/generated/gate/messages/publicTrade.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"
#include "exchange/gate/sbe/message_dispatcher.h"

namespace aquila::gate {

namespace detail {

inline constexpr std::uint16_t kPublicTradeBlockLength =
    ::sbepp::message_traits<
        ::gate::schema::messages::publicTrade>::block_length();
inline constexpr size_t kMinPublicTradePayloadBytes =
    kSbeMessageHeaderBytes + kPublicTradeBlockLength + 4 + 2;

inline bool IsPublicTradeHeader(const SbeMessageHeader& header) noexcept {
  return header.block_length == kPublicTradeBlockLength &&
         header.template_id == kGateSbePublicTradeTemplateId &&
         header.schema_id == kGateSbeSchemaId &&
         header.version == kGateSbeSchemaVersion;
}

template <typename TradeEntry>
inline void AssignTradeFromView(const TradeEntry& entry,
                                std::int64_t exchange_ns, std::int64_t local_ns,
                                std::int32_t symbol_id, double price_scale,
                                double volume_scale, std::uint32_t batch_index,
                                std::uint32_t batch_count,
                                Trade& out) noexcept {
  const std::int64_t size = entry.size().value();
  out.id = static_cast<std::int64_t>(entry.id().value());
  out.symbol_id = symbol_id;
  out.exchange = Exchange::kGate;
  out.side = size >= 0 ? OrderSide::kBuy : OrderSide::kSell;
  out.reserved = 0;
  out.exchange_ns = exchange_ns;
  out.event_ns = entry.t().value() * 1000;
  out.local_ns = local_ns;
  out.price = static_cast<double>(entry.price().value()) * price_scale;
  out.volume = static_cast<double>(std::llabs(size)) * volume_scale;
  out.batch_index = batch_index;
  out.batch_count = batch_count;
}

}  // namespace detail

inline std::string_view ExtractTrustedTradeSymbol(
    std::string_view payload, const SbeMessageHeader& header) noexcept {
  assert(detail::IsPublicTradeHeader(header));
  assert(payload.size() >= detail::kMinPublicTradePayloadBytes);

  const auto view = ::sbepp::make_const_view<::gate::messages::publicTrade>(
      payload.data(), payload.size());
  size_t offset =
      kSbeMessageHeaderBytes + detail::kPublicTradeBlockLength + 4 +
      view.trades().size() *
          ::sbepp::group_traits<
              ::gate::schema::messages::publicTrade::trades>::block_length();
  std::string_view channel;
  std::string_view symbol;
  [[maybe_unused]] bool ok = detail::ReadVarString8(payload, offset, channel);
  assert(ok);
  ok = detail::ReadVarString8(payload, offset, symbol);
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
  assert(payload.size() >= detail::kMinPublicTradePayloadBytes);
  assert(detail::IsPublicTradeHeader(header));

  const auto view = ::sbepp::make_const_view<::gate::messages::publicTrade>(
      payload.data(), payload.size());
  assert(view.e() == ::gate::types::Event::Update);

  const std::int64_t exchange_ns = view.time().value() * 1000;
  const double price_scale =
      detail::DecimalExponentScale(view.pxExponent().value());
  const double volume_scale =
      detail::DecimalExponentScale(view.szExponent().value());
  const auto trades = view.trades();
  const auto batch_count = static_cast<std::uint32_t>(trades.size());
  std::uint32_t batch_index = 0;
  for (const auto entry : trades) {
    std::forward<WriterFactory>(writer_factory)([&](Trade& out) noexcept {
      detail::AssignTradeFromView(entry, exchange_ns, local_ns, symbol_id,
                                  price_scale, volume_scale, batch_index,
                                  batch_count, out);
    });
    ++batch_index;
  }
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_SBE_TRADE_DECODER_H_
