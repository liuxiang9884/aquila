#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_FEEDBACK_PARSER_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_FEEDBACK_PARSER_H_

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include <sbepp/sbepp.hpp>

#include "core/trading/order_feedback_event.h"
#include "core/trading/order_id.h"
#include "exchange/gate/sbe/generated/gate/messages/orders.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"
#include "exchange/gate/sbe/message_dispatcher.h"
#include "exchange/gate/trading/order_codecs.h"

namespace aquila::gate {

enum class OrderFeedbackParseStatus : std::uint8_t {
  kOk = 0,
  kNeedMore,
  kUnsupportedSchema,
  kUnsupportedSchemaVersion,
  kUnsupportedTemplate,
  kUnexpectedBlockLength,
  kUnexpectedEvent,
  kUnexpectedChannel,
  kMalformedPayload,
};

struct OrderFeedbackParserStats {
  std::uint64_t messages_seen{0};
  std::uint64_t messages_parsed{0};
  std::uint64_t orders_seen{0};
  std::uint64_t events_emitted{0};
  std::uint64_t dropped_events{0};

  std::uint64_t need_more_count{0};
  std::uint64_t unsupported_schema_count{0};
  std::uint64_t unsupported_schema_version_count{0};
  std::uint64_t unsupported_template_count{0};
  std::uint64_t unexpected_block_length_count{0};
  std::uint64_t unexpected_event_count{0};
  std::uint64_t unexpected_channel_count{0};
  std::uint64_t malformed_payload_count{0};

  std::uint64_t invalid_text_count{0};
  std::uint64_t invalid_route_count{0};
  std::uint64_t unsupported_size_exponent_count{0};
  std::uint64_t unsupported_filled_left_count{0};
  std::uint64_t unsupported_finish_as_count{0};
  std::uint64_t unsupported_price_exponent_count{0};
  std::uint64_t invalid_quantity_count{0};
};

struct OrderFeedbackParseResult {
  OrderFeedbackParseStatus status{OrderFeedbackParseStatus::kOk};
  std::uint16_t orders_seen{0};
  std::uint16_t events_emitted{0};
};

enum class OrderFeedbackRawUpdateOutcome : std::uint8_t {
  kEmitted = 0,
  kDroppedInvalidText,
  kDroppedInvalidRoute,
  kDroppedUnsupportedSizeExponent,
  kDroppedInvalidQuantity,
  kDroppedMalformedUpdateTime,
  kDroppedUnsupportedPriceExponent,
  kDroppedUnsupportedUpdateFinishAs,
  kDroppedUnsupportedFilledLeft,
  kDroppedUnsupportedFinishAs,
};

struct OrderFeedbackRawUpdateDiagnostic {
  std::uint16_t update_index{0};
  std::uint16_t result_count{0};
  std::uint64_t exchange_order_id{0};
  std::int8_t size_exponent{0};
  std::int64_t left_mantissa{0};
  std::int64_t size_mantissa{0};
  std::int64_t update_time_us{0};
  std::int8_t price_exponent{0};
  std::int64_t fill_price_mantissa{0};
  std::string_view role{};
  std::string_view text{};
  std::string_view finish_as{};
  bool local_order_id_valid{false};
  std::uint64_t local_order_id{0};
  double size_quantity{0.0};
  double left_quantity{0.0};
  double fill_price{0.0};
  std::int64_t exchange_update_ns{0};
  OrderFeedbackRawUpdateOutcome outcome{
      OrderFeedbackRawUpdateOutcome::kDroppedUnsupportedFinishAs};
  bool event_emitted{false};
  OrderFeedbackKind emit_kind{OrderFeedbackKind::kContinuityLost};
  bool publish_ok{false};
};

namespace detail {

inline constexpr std::uint8_t kOrderFeedbackMaxStrategyCount = 8;
inline constexpr std::string_view kOrdersChannel = "futures.orders";
inline constexpr std::uint16_t kOrdersMessageBlockLength =
    ::sbepp::message_traits<::gate::schema::messages::orders>::block_length();
inline constexpr std::uint16_t kOrdersResultBlockLength = ::sbepp::group_traits<
    ::gate::schema::messages::orders::result>::block_length();
inline constexpr std::size_t kOrdersGroupHeaderBytes = 4;
inline constexpr std::size_t kOrdersMinPayloadBytes =
    kSbeMessageHeaderBytes + kOrdersMessageBlockLength +
    kOrdersGroupHeaderBytes;
static_assert(kOrdersMessageBlockLength == 9);
static_assert(kOrdersResultBlockLength == 156);

template <typename T>
inline bool ReadLittleEndian(std::string_view payload, std::size_t offset,
                             T* out) noexcept {
  static_assert(std::is_integral_v<T>);
  assert(out != nullptr);
  if (offset > payload.size() || payload.size() - offset < sizeof(T)) {
    return false;
  }
  std::memcpy(out, payload.data() + offset, sizeof(T));
  return true;
}

inline bool ReadVarString8(std::string_view payload, std::size_t* offset,
                           std::string_view* out) noexcept {
  assert(offset != nullptr);
  assert(out != nullptr);
  if (*offset >= payload.size()) {
    return false;
  }

  const std::size_t length =
      static_cast<unsigned char>(payload.data()[(*offset)++]);
  if (payload.size() - *offset < length) {
    return false;
  }

  *out = payload.substr(*offset, length);
  *offset += length;
  return true;
}

inline bool TryMicrosToNanos(std::int64_t micros,
                             std::int64_t* nanos) noexcept {
  assert(nanos != nullptr);
  if (micros > std::numeric_limits<std::int64_t>::max() / 1000 ||
      micros < std::numeric_limits<std::int64_t>::min() / 1000) {
    return false;
  }
  *nanos = micros * 1000;
  return true;
}

inline bool TryAbsInt64(std::int64_t value, std::int64_t* out) noexcept {
  assert(out != nullptr);
  if (value == std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  *out = value < 0 ? -value : value;
  return true;
}

inline bool IsSupportedDecimalExponent(std::int8_t exponent) noexcept {
  return exponent >= -15 && exponent <= 15;
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
      0.00000000001,
      0.000000000001,
      0.0000000000001,
      0.00000000000001,
      0.000000000000001,
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
      100'000'000'000.0,
      1'000'000'000'000.0,
      10'000'000'000'000.0,
      100'000'000'000'000.0,
      1'000'000'000'000'000.0,
  };

  assert(IsSupportedDecimalExponent(exponent));
  const int exponent_value = exponent;
  if (exponent_value <= 0) {
    return kNegativePowersOfTen[-exponent_value];
  }
  return kPositivePowersOfTen[exponent_value];
}

inline bool TryDecimalMantissaToQuantity(std::int64_t mantissa,
                                         std::int8_t exponent,
                                         double* out) noexcept {
  assert(out != nullptr);
  std::int64_t abs_mantissa = 0;
  if (!TryAbsInt64(mantissa, &abs_mantissa)) {
    return false;
  }
  *out = static_cast<double>(abs_mantissa) * DecimalExponentScale(exponent);
  return std::isfinite(*out);
}

inline OrderRole ParseOrderRole(std::string_view role) noexcept {
  if (role == std::string_view("maker")) {
    return OrderRole::kMaker;
  }
  if (role == std::string_view("taker")) {
    return OrderRole::kTaker;
  }
  return OrderRole::kNone;
}

inline bool ParseTerminalFinishReason(std::string_view finish_as,
                                      OrderFinishReason* reason) noexcept {
  assert(reason != nullptr);
  if (finish_as == std::string_view("cancelled")) {
    *reason = OrderFinishReason::kManualCancelled;
    return true;
  }
  if (finish_as == std::string_view("ioc")) {
    *reason = OrderFinishReason::kImmediateOrCancel;
    return true;
  }
  if (finish_as == std::string_view("reduce_only")) {
    *reason = OrderFinishReason::kReduceOnly;
    return true;
  }
  if (finish_as == std::string_view("reduce_out")) {
    *reason = OrderFinishReason::kReduceOut;
    return true;
  }
  if (finish_as == std::string_view("stp")) {
    *reason = OrderFinishReason::kSelfTradePrevention;
    return true;
  }
  if (finish_as == std::string_view("liquidated")) {
    *reason = OrderFinishReason::kLiquidated;
    return true;
  }
  if (finish_as == std::string_view("auto_deleveraging")) {
    *reason = OrderFinishReason::kAutoDeleveraging;
    return true;
  }
  if (finish_as == std::string_view("position_close")) {
    *reason = OrderFinishReason::kPositionClose;
    return true;
  }
  return false;
}

struct RawOrderFeedbackUpdate {
  std::uint64_t exchange_order_id{0};
  std::int8_t size_exponent{0};
  std::int64_t left_mantissa{0};
  std::int64_t size_mantissa{0};
  std::int64_t update_time_us{0};
  std::int8_t price_exponent{0};
  std::int64_t fill_price_mantissa{0};
  std::string_view role{};
  std::string_view text{};
  std::string_view finish_as{};
};

struct NoopOrderFeedbackRawUpdateObserver {
  void operator()(const OrderFeedbackRawUpdateDiagnostic&) const noexcept {}
};

inline OrderFeedbackRawUpdateDiagnostic MakeRawUpdateDiagnostic(
    const RawOrderFeedbackUpdate& raw, std::uint16_t update_index,
    std::uint16_t result_count) noexcept {
  return OrderFeedbackRawUpdateDiagnostic{
      .update_index = update_index,
      .result_count = result_count,
      .exchange_order_id = raw.exchange_order_id,
      .size_exponent = raw.size_exponent,
      .left_mantissa = raw.left_mantissa,
      .size_mantissa = raw.size_mantissa,
      .update_time_us = raw.update_time_us,
      .price_exponent = raw.price_exponent,
      .fill_price_mantissa = raw.fill_price_mantissa,
      .role = raw.role,
      .text = raw.text,
      .finish_as = raw.finish_as,
  };
}

inline bool ReadRawOrderFeedbackUpdate(std::string_view payload,
                                       std::size_t* offset,
                                       RawOrderFeedbackUpdate* out) noexcept {
  assert(offset != nullptr);
  assert(out != nullptr);
  if (*offset > payload.size() ||
      payload.size() - *offset < kOrdersResultBlockLength) {
    return false;
  }

  const std::size_t fixed_offset = *offset;
  if (!ReadLittleEndian<std::uint64_t>(payload, fixed_offset + 8,
                                       &out->exchange_order_id) ||
      !ReadLittleEndian<std::int8_t>(payload, fixed_offset + 16,
                                     &out->size_exponent) ||
      !ReadLittleEndian<std::int64_t>(payload, fixed_offset + 25,
                                      &out->left_mantissa) ||
      !ReadLittleEndian<std::int64_t>(payload, fixed_offset + 33,
                                      &out->size_mantissa) ||
      !ReadLittleEndian<std::int64_t>(payload, fixed_offset + 66,
                                      &out->update_time_us) ||
      !ReadLittleEndian<std::int8_t>(payload, fixed_offset + 87,
                                     &out->price_exponent) ||
      !ReadLittleEndian<std::int64_t>(payload, fixed_offset + 88,
                                      &out->fill_price_mantissa)) {
    return false;
  }
  *offset += kOrdersResultBlockLength;

  std::string_view ignored;
  return ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &out->role) &&
         ReadVarString8(payload, offset, &out->text) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &out->finish_as) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored);
}

inline bool SkipRawOrderFeedbackUpdate(std::string_view payload,
                                       std::size_t* offset) noexcept {
  assert(offset != nullptr);
  if (*offset > payload.size() ||
      payload.size() - *offset < kOrdersResultBlockLength) {
    return false;
  }
  *offset += kOrdersResultBlockLength;

  std::string_view ignored;
  return ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored) &&
         ReadVarString8(payload, offset, &ignored);
}

template <typename EventSink>
inline bool EmitOrderFeedbackEvent(OrderFeedbackEvent& event,
                                   OrderFeedbackParserStats& stats,
                                   OrderFeedbackParseResult& result,
                                   EventSink& sink) noexcept {
  bool publish_ok = true;
  using SinkResult =
      std::invoke_result_t<EventSink&, const OrderFeedbackEvent&>;
  if constexpr (std::is_void_v<SinkResult>) {
    sink(event);
  } else {
    publish_ok = static_cast<bool>(sink(event));
  }
  ++stats.events_emitted;
  ++result.events_emitted;
  return publish_ok;
}

template <typename RawUpdateSink>
inline void NotifyDroppedRawUpdate(OrderFeedbackRawUpdateDiagnostic& diagnostic,
                                   OrderFeedbackRawUpdateOutcome outcome,
                                   OrderFeedbackParserStats& stats,
                                   RawUpdateSink& raw_update_sink) noexcept {
  ++stats.dropped_events;
  diagnostic.outcome = outcome;
  raw_update_sink(diagnostic);
}

template <typename EventSink, typename RawUpdateSink>
inline void ConvertRawOrderFeedbackUpdate(
    const RawOrderFeedbackUpdate& raw, std::uint16_t update_index,
    std::uint16_t result_count, std::int64_t local_receive_ns,
    OrderFeedbackParserStats& stats, OrderFeedbackParseResult& result,
    EventSink& sink, RawUpdateSink& raw_update_sink) noexcept {
  OrderFeedbackRawUpdateDiagnostic diagnostic =
      MakeRawUpdateDiagnostic(raw, update_index, result_count);

  const ParsedOrderText parsed_text = OrderTextCodec::Parse(raw.text);
  if (!parsed_text.ok) {
    ++stats.invalid_text_count;
    NotifyDroppedRawUpdate(diagnostic,
                           OrderFeedbackRawUpdateOutcome::kDroppedInvalidText,
                           stats, raw_update_sink);
    return;
  }
  diagnostic.local_order_id_valid = true;
  diagnostic.local_order_id = parsed_text.local_order_id;

  if (LocalOrderIdCodec::StrategyId(parsed_text.local_order_id) >=
      kOrderFeedbackMaxStrategyCount) {
    ++stats.invalid_route_count;
    NotifyDroppedRawUpdate(diagnostic,
                           OrderFeedbackRawUpdateOutcome::kDroppedInvalidRoute,
                           stats, raw_update_sink);
    return;
  }

  if (!IsSupportedDecimalExponent(raw.size_exponent)) {
    ++stats.unsupported_size_exponent_count;
    NotifyDroppedRawUpdate(
        diagnostic,
        OrderFeedbackRawUpdateOutcome::kDroppedUnsupportedSizeExponent, stats,
        raw_update_sink);
    return;
  }

  double size_quantity = 0.0;
  double left_quantity = 0.0;
  if (!TryDecimalMantissaToQuantity(raw.size_mantissa, raw.size_exponent,
                                    &size_quantity) ||
      !TryDecimalMantissaToQuantity(raw.left_mantissa, raw.size_exponent,
                                    &left_quantity) ||
      left_quantity > size_quantity + 1e-12) {
    ++stats.invalid_quantity_count;
    NotifyDroppedRawUpdate(
        diagnostic, OrderFeedbackRawUpdateOutcome::kDroppedInvalidQuantity,
        stats, raw_update_sink);
    return;
  }
  diagnostic.size_quantity = size_quantity;
  diagnostic.left_quantity = left_quantity;

  std::int64_t exchange_update_ns = 0;
  if (!TryMicrosToNanos(raw.update_time_us, &exchange_update_ns)) {
    ++stats.malformed_payload_count;
    NotifyDroppedRawUpdate(
        diagnostic, OrderFeedbackRawUpdateOutcome::kDroppedMalformedUpdateTime,
        stats, raw_update_sink);
    return;
  }
  diagnostic.exchange_update_ns = exchange_update_ns;

  if (!IsSupportedDecimalExponent(raw.price_exponent)) {
    ++stats.unsupported_price_exponent_count;
    NotifyDroppedRawUpdate(
        diagnostic,
        OrderFeedbackRawUpdateOutcome::kDroppedUnsupportedPriceExponent, stats,
        raw_update_sink);
    return;
  }

  const double cumulative_filled_quantity = size_quantity - left_quantity;
  const double fill_price = static_cast<double>(raw.fill_price_mantissa) *
                            DecimalExponentScale(raw.price_exponent);
  diagnostic.fill_price = fill_price;

  OrderFeedbackEvent event{};
  event.local_order_id = parsed_text.local_order_id;
  event.exchange_order_id = raw.exchange_order_id;
  event.exchange_update_ns = exchange_update_ns;
  event.local_receive_ns = local_receive_ns;
  event.cumulative_filled_quantity = cumulative_filled_quantity;
  event.left_quantity = left_quantity;
  event.fill_price = fill_price;

  const auto emit_event = [&](OrderFeedbackKind kind) noexcept {
    event.kind = kind;
    diagnostic.outcome = OrderFeedbackRawUpdateOutcome::kEmitted;
    diagnostic.event_emitted = true;
    diagnostic.emit_kind = kind;
    diagnostic.publish_ok = EmitOrderFeedbackEvent(event, stats, result, sink);
    raw_update_sink(diagnostic);
  };

  if (raw.finish_as == std::string_view("_new")) {
    emit_event(OrderFeedbackKind::kAccepted);
    return;
  }

  if (raw.finish_as == std::string_view("_update")) {
    if (left_quantity <= 0.0) {
      ++stats.unsupported_finish_as_count;
      NotifyDroppedRawUpdate(
          diagnostic,
          OrderFeedbackRawUpdateOutcome::kDroppedUnsupportedUpdateFinishAs,
          stats, raw_update_sink);
      return;
    }
    emit_event(OrderFeedbackKind::kPartialFilled);
    return;
  }

  if (raw.finish_as == std::string_view("filled")) {
    if (std::abs(left_quantity) > 1e-12) {
      ++stats.unsupported_filled_left_count;
      NotifyDroppedRawUpdate(
          diagnostic,
          OrderFeedbackRawUpdateOutcome::kDroppedUnsupportedFilledLeft, stats,
          raw_update_sink);
      return;
    }
    event.role = ParseOrderRole(raw.role);
    emit_event(OrderFeedbackKind::kFilled);
    return;
  }

  OrderFinishReason reason = OrderFinishReason::kUnknown;
  if (ParseTerminalFinishReason(raw.finish_as, &reason)) {
    event.cancelled_quantity = left_quantity;
    event.finish_reason = reason;
    emit_event(OrderFeedbackKind::kCancelled);
    return;
  }

  ++stats.unsupported_finish_as_count;
  NotifyDroppedRawUpdate(
      diagnostic, OrderFeedbackRawUpdateOutcome::kDroppedUnsupportedFinishAs,
      stats, raw_update_sink);
}

inline OrderFeedbackParseStatus MapDispatchStatus(
    SbeDispatchStatus status) noexcept {
  switch (status) {
    case SbeDispatchStatus::kReady:
      return OrderFeedbackParseStatus::kOk;
    case SbeDispatchStatus::kNeedMore:
      return OrderFeedbackParseStatus::kNeedMore;
    case SbeDispatchStatus::kUnsupportedSchema:
      return OrderFeedbackParseStatus::kUnsupportedSchema;
    case SbeDispatchStatus::kUnsupportedSchemaVersion:
      return OrderFeedbackParseStatus::kUnsupportedSchemaVersion;
    case SbeDispatchStatus::kUnsupportedTemplate:
      return OrderFeedbackParseStatus::kUnsupportedTemplate;
  }
  return OrderFeedbackParseStatus::kMalformedPayload;
}

inline void CountDispatchFailure(OrderFeedbackParseStatus status,
                                 OrderFeedbackParserStats& stats) noexcept {
  switch (status) {
    case OrderFeedbackParseStatus::kNeedMore:
      ++stats.need_more_count;
      return;
    case OrderFeedbackParseStatus::kUnsupportedSchema:
      ++stats.unsupported_schema_count;
      return;
    case OrderFeedbackParseStatus::kUnsupportedSchemaVersion:
      ++stats.unsupported_schema_version_count;
      return;
    case OrderFeedbackParseStatus::kUnsupportedTemplate:
      ++stats.unsupported_template_count;
      return;
    case OrderFeedbackParseStatus::kOk:
    case OrderFeedbackParseStatus::kUnexpectedBlockLength:
    case OrderFeedbackParseStatus::kUnexpectedEvent:
    case OrderFeedbackParseStatus::kUnexpectedChannel:
    case OrderFeedbackParseStatus::kMalformedPayload:
      return;
  }
}

struct OrdersPayloadFrame {
  std::size_t updates_offset{0};
  std::uint16_t result_count{0};
};

inline OrderFeedbackParseStatus ValidateOrdersPayloadHeader(
    std::string_view payload, const SbeDispatchResult& dispatch,
    OrderFeedbackParserStats& stats, OrdersPayloadFrame* frame) noexcept {
  assert(frame != nullptr);
  if (dispatch.header.block_length != kOrdersMessageBlockLength ||
      payload.size() < kOrdersMinPayloadBytes) {
    if (payload.size() < kOrdersMinPayloadBytes) {
      ++stats.need_more_count;
      return OrderFeedbackParseStatus::kNeedMore;
    }
    ++stats.unexpected_block_length_count;
    return OrderFeedbackParseStatus::kUnexpectedBlockLength;
  }

  std::int8_t event_type = 0;
  if (!ReadLittleEndian<std::int8_t>(
          payload, kSbeMessageHeaderBytes + sizeof(std::int64_t),
          &event_type)) {
    ++stats.need_more_count;
    return OrderFeedbackParseStatus::kNeedMore;
  }
  if (event_type != static_cast<std::int8_t>(::gate::types::Event::Update)) {
    ++stats.unexpected_event_count;
    return OrderFeedbackParseStatus::kUnexpectedEvent;
  }

  std::size_t offset = kSbeMessageHeaderBytes + kOrdersMessageBlockLength;
  std::uint16_t result_block_length = 0;
  std::uint16_t result_count = 0;
  if (!ReadLittleEndian<std::uint16_t>(payload, offset, &result_block_length) ||
      !ReadLittleEndian<std::uint16_t>(payload, offset + sizeof(std::uint16_t),
                                       &result_count)) {
    ++stats.need_more_count;
    return OrderFeedbackParseStatus::kNeedMore;
  }
  offset += kOrdersGroupHeaderBytes;

  if (result_block_length != kOrdersResultBlockLength) {
    ++stats.unexpected_block_length_count;
    return OrderFeedbackParseStatus::kUnexpectedBlockLength;
  }

  frame->updates_offset = offset;
  frame->result_count = result_count;
  return OrderFeedbackParseStatus::kOk;
}

inline OrderFeedbackParseStatus ValidateOrdersPayloadTail(
    std::string_view payload, std::size_t* offset,
    OrderFeedbackParserStats& stats) noexcept {
  assert(offset != nullptr);
  std::string_view channel;
  if (!ReadVarString8(payload, offset, &channel)) {
    ++stats.malformed_payload_count;
    return OrderFeedbackParseStatus::kMalformedPayload;
  }
  if (channel != kOrdersChannel) {
    ++stats.unexpected_channel_count;
    return OrderFeedbackParseStatus::kUnexpectedChannel;
  }
  if (*offset != payload.size()) {
    ++stats.malformed_payload_count;
    return OrderFeedbackParseStatus::kMalformedPayload;
  }
  return OrderFeedbackParseStatus::kOk;
}

}  // namespace detail

template <typename EventSink, typename RawUpdateSink>
inline OrderFeedbackParseResult ParseGateOrderFeedbackMessage(
    std::string_view payload, std::int64_t local_receive_ns,
    OrderFeedbackParserStats& stats, EventSink&& sink,
    RawUpdateSink&& raw_update_sink) noexcept {
  ++stats.messages_seen;

  OrderFeedbackParseResult result{};
  const SbeDispatchResult dispatch = DispatchSbeMessage(payload);
  if (dispatch.status != SbeDispatchStatus::kReady ||
      dispatch.message_type != GateSbeMessageType::kOrders) {
    result.status = dispatch.status == SbeDispatchStatus::kReady
                        ? OrderFeedbackParseStatus::kUnsupportedTemplate
                        : detail::MapDispatchStatus(dispatch.status);
    detail::CountDispatchFailure(result.status, stats);
    return result;
  }

  detail::OrdersPayloadFrame frame{};
  result.status =
      detail::ValidateOrdersPayloadHeader(payload, dispatch, stats, &frame);
  if (result.status != OrderFeedbackParseStatus::kOk) {
    return result;
  }

  if (frame.result_count == 1) {
    detail::RawOrderFeedbackUpdate raw{};
    std::size_t offset = frame.updates_offset;
    if (!detail::ReadRawOrderFeedbackUpdate(payload, &offset, &raw)) {
      result.status = OrderFeedbackParseStatus::kMalformedPayload;
      ++stats.malformed_payload_count;
      return result;
    }
    result.status = detail::ValidateOrdersPayloadTail(payload, &offset, stats);
    if (result.status != OrderFeedbackParseStatus::kOk) {
      return result;
    }

    ++stats.orders_seen;
    ++result.orders_seen;
    detail::ConvertRawOrderFeedbackUpdate(raw, /*update_index=*/0,
                                          frame.result_count, local_receive_ns,
                                          stats, result, sink, raw_update_sink);
    ++stats.messages_parsed;
    return result;
  }

  std::size_t validation_offset = frame.updates_offset;
  for (std::uint16_t i = 0; i < frame.result_count; ++i) {
    if (!detail::SkipRawOrderFeedbackUpdate(payload, &validation_offset)) {
      result.status = OrderFeedbackParseStatus::kMalformedPayload;
      ++stats.malformed_payload_count;
      return result;
    }
  }
  result.status =
      detail::ValidateOrdersPayloadTail(payload, &validation_offset, stats);
  if (result.status != OrderFeedbackParseStatus::kOk) {
    return result;
  }

  std::size_t offset = frame.updates_offset;
  for (std::uint16_t i = 0; i < frame.result_count; ++i) {
    detail::RawOrderFeedbackUpdate raw{};
    if (!detail::ReadRawOrderFeedbackUpdate(payload, &offset, &raw)) {
      result.status = OrderFeedbackParseStatus::kMalformedPayload;
      ++stats.malformed_payload_count;
      return result;
    }

    ++stats.orders_seen;
    ++result.orders_seen;
    detail::ConvertRawOrderFeedbackUpdate(raw, i, frame.result_count,
                                          local_receive_ns, stats, result, sink,
                                          raw_update_sink);
  }

  ++stats.messages_parsed;
  return result;
}

template <typename EventSink>
inline OrderFeedbackParseResult ParseGateOrderFeedbackMessage(
    std::string_view payload, std::int64_t local_receive_ns,
    OrderFeedbackParserStats& stats, EventSink&& sink) noexcept {
  detail::NoopOrderFeedbackRawUpdateObserver raw_update_sink{};
  return ParseGateOrderFeedbackMessage(payload, local_receive_ns, stats,
                                       std::forward<EventSink>(sink),
                                       raw_update_sink);
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_FEEDBACK_PARSER_H_
