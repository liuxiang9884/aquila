#ifndef AQUILA_EVALUATION_EXCHANGE_GATE_TRADING_ORDER_FEEDBACK_PAYLOAD_BUILDER_H_
#define AQUILA_EVALUATION_EXCHANGE_GATE_TRADING_ORDER_FEEDBACK_PAYLOAD_BUILDER_H_

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <sbepp/sbepp.hpp>

#include "core/trading/order_id.h"
#include "exchange/gate/sbe/generated/gate/messages/orders.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"
#include "exchange/gate/sbe/message_dispatcher.h"

namespace aquila::gate::evaluation {

inline constexpr std::uint8_t kOrderFeedbackPayloadStrategyId = 3;
inline constexpr std::uint64_t kOrderFeedbackPayloadLocalOrderId =
    LocalOrderIdCodec::Encode(kOrderFeedbackPayloadStrategyId, 42);
inline constexpr std::uint64_t kOrderFeedbackPayloadExchangeOrderId =
    36'028'827'892'199'865ULL;
inline constexpr std::int64_t kOrderFeedbackPayloadUpdateTimeUs =
    1'770'000'000'001'111;

struct OrderFeedbackPayloadFields {
  std::uint64_t exchange_order_id{kOrderFeedbackPayloadExchangeOrderId};
  std::int8_t size_exponent{0};
  std::int64_t left_mantissa{4};
  std::int64_t size_mantissa{10};
  std::int64_t update_time_us{kOrderFeedbackPayloadUpdateTimeUs};
  std::int8_t price_exponent{-2};
  std::int64_t fill_price_mantissa{6'501'250};
  std::string_view role{};
  std::string_view text{"t-216172782113783850"};
  std::string_view finish_as{"_new"};
};

namespace order_feedback_payload_detail {

template <typename T, std::size_t N>
void WriteLittleEndian(std::array<char, N>& buffer, std::size_t offset,
                       T value) noexcept {
  assert(offset + sizeof(value) <= buffer.size());
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

template <std::size_t N>
void AppendVarString(std::array<char, N>& buffer, std::size_t* offset,
                     std::string_view value) noexcept {
  assert(offset != nullptr);
  assert(value.size() <= 255);
  assert(*offset + 1 + value.size() <= buffer.size());
  buffer[(*offset)++] = static_cast<char>(value.size());
  std::memcpy(buffer.data() + *offset, value.data(), value.size());
  *offset += value.size();
}

template <std::size_t N>
void WriteOrderFeedbackEntry(std::array<char, N>& buffer, std::size_t* offset,
                             const OrderFeedbackPayloadFields& fields,
                             std::uint16_t result_block_length) noexcept {
  assert(offset != nullptr);
  assert(*offset + result_block_length <= buffer.size());

  const std::size_t entry_offset = *offset;
  WriteLittleEndian<std::int64_t>(buffer, entry_offset + 0,
                                  kOrderFeedbackPayloadUpdateTimeUs - 10);
  WriteLittleEndian<std::uint64_t>(buffer, entry_offset + 8,
                                   fields.exchange_order_id);
  WriteLittleEndian<std::int8_t>(buffer, entry_offset + 16,
                                 fields.size_exponent);
  WriteLittleEndian<std::int64_t>(buffer, entry_offset + 25,
                                  fields.left_mantissa);
  WriteLittleEndian<std::int64_t>(buffer, entry_offset + 33,
                                  fields.size_mantissa);
  WriteLittleEndian<std::int64_t>(buffer, entry_offset + 57, 0);
  WriteLittleEndian<std::int8_t>(buffer, entry_offset + 65, 1);
  WriteLittleEndian<std::int64_t>(buffer, entry_offset + 66,
                                  fields.update_time_us);
  WriteLittleEndian<std::int8_t>(buffer, entry_offset + 87,
                                 fields.price_exponent);
  WriteLittleEndian<std::int64_t>(buffer, entry_offset + 88,
                                  fields.fill_price_mantissa);
  WriteLittleEndian<std::int64_t>(buffer, entry_offset + 96,
                                  fields.fill_price_mantissa);
  *offset += result_block_length;

  AppendVarString(buffer, offset, "BTC_USDT");
  AppendVarString(buffer, offset, fields.role);
  AppendVarString(buffer, offset, fields.text);
  AppendVarString(buffer, offset, "gtc");
  AppendVarString(buffer, offset, fields.finish_as);
  AppendVarString(buffer, offset, "open");
  AppendVarString(buffer, offset, "");
  AppendVarString(buffer, offset, "");
  AppendVarString(buffer, offset, "");
  AppendVarString(buffer, offset, "");
  AppendVarString(buffer, offset, "");
  AppendVarString(buffer, offset, "");
  AppendVarString(buffer, offset, "100");
}

}  // namespace order_feedback_payload_detail

template <std::size_t N>
std::string_view BuildOrderFeedbackOrdersPayload(
    std::array<char, N>* buffer,
    std::span<const OrderFeedbackPayloadFields> updates,
    std::string_view channel = "futures.orders") noexcept {
  assert(buffer != nullptr);
  assert(updates.size() <= 65535);
  static constexpr std::uint16_t kMessageBlockLength =
      ::sbepp::message_traits<::gate::schema::messages::orders>::block_length();
  static constexpr std::uint16_t kResultBlockLength = ::sbepp::group_traits<
      ::gate::schema::messages::orders::result>::block_length();
  static_assert(kMessageBlockLength == 9);
  static_assert(kResultBlockLength == 156);

  std::size_t offset = 0;
  order_feedback_payload_detail::WriteLittleEndian<std::uint16_t>(
      *buffer, offset, kMessageBlockLength);
  offset += sizeof(std::uint16_t);
  order_feedback_payload_detail::WriteLittleEndian<std::uint16_t>(
      *buffer, offset, kGateSbeOrdersTemplateId);
  offset += sizeof(std::uint16_t);
  order_feedback_payload_detail::WriteLittleEndian<std::uint16_t>(
      *buffer, offset, kGateSbeSchemaId);
  offset += sizeof(std::uint16_t);
  order_feedback_payload_detail::WriteLittleEndian<std::uint16_t>(
      *buffer, offset, kGateSbeSchemaVersion);
  offset += sizeof(std::uint16_t);

  order_feedback_payload_detail::WriteLittleEndian<std::int64_t>(
      *buffer, offset, kOrderFeedbackPayloadUpdateTimeUs);
  offset += sizeof(std::int64_t);
  order_feedback_payload_detail::WriteLittleEndian<std::int8_t>(
      *buffer, offset, static_cast<std::int8_t>(::gate::types::Event::Update));
  offset += sizeof(std::int8_t);

  order_feedback_payload_detail::WriteLittleEndian<std::uint16_t>(
      *buffer, offset, kResultBlockLength);
  offset += sizeof(std::uint16_t);
  order_feedback_payload_detail::WriteLittleEndian<std::uint16_t>(
      *buffer, offset, static_cast<std::uint16_t>(updates.size()));
  offset += sizeof(std::uint16_t);

  for (const OrderFeedbackPayloadFields& fields : updates) {
    order_feedback_payload_detail::WriteOrderFeedbackEntry(
        *buffer, &offset, fields, kResultBlockLength);
  }
  order_feedback_payload_detail::AppendVarString(*buffer, &offset, channel);
  return {buffer->data(), offset};
}

template <std::size_t N>
std::string_view BuildOrderFeedbackOrdersPayload(
    std::array<char, N>* buffer, const OrderFeedbackPayloadFields& fields = {},
    std::string_view channel = "futures.orders") noexcept {
  return BuildOrderFeedbackOrdersPayload(
      buffer, std::span<const OrderFeedbackPayloadFields>(&fields, 1), channel);
}

template <std::size_t N, std::size_t UpdateCount>
std::string_view BuildOrderFeedbackOrdersPayload(
    std::array<char, N>* buffer,
    const std::array<OrderFeedbackPayloadFields, UpdateCount>& updates,
    std::string_view channel = "futures.orders") noexcept {
  return BuildOrderFeedbackOrdersPayload(
      buffer,
      std::span<const OrderFeedbackPayloadFields>(updates.data(),
                                                  updates.size()),
      channel);
}

}  // namespace aquila::gate::evaluation

#endif  // AQUILA_EVALUATION_EXCHANGE_GATE_TRADING_ORDER_FEEDBACK_PAYLOAD_BUILDER_H_
