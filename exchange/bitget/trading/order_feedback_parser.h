#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ORDER_FEEDBACK_PARSER_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ORDER_FEEDBACK_PARSER_H_

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <fast_float/fast_float.h>

#include "core/trading/order_feedback_event.h"
#include "exchange/bitget/trading/order_codecs.h"
#include "exchange/common/simdjson_utils.h"
#include <simdjson.h>

namespace aquila::bitget {

enum class OrderFeedbackParseStatus : std::uint8_t {
  kOk,
  kControlMessage,
  kFastFillMessage,
  kInvalidJson,
  kUnexpectedEnvelope,
  kDecodeUnrecoverable,
};

struct OrderFeedbackParserStats {
  std::uint64_t messages_seen{0};
  std::uint64_t messages_parsed{0};
  std::uint64_t order_envelopes{0};
  std::uint64_t orders_seen{0};
  std::uint64_t events_emitted{0};
  std::uint64_t foreign_orders_ignored{0};
  std::uint64_t foreign_run_namespace_orders_ignored{0};
  std::uint64_t legacy_client_oid_orders_ignored{0};
  std::uint64_t unroutable_orders_ignored{0};
  std::uint64_t legacy_canceled_statuses{0};
  std::uint64_t validation_errors{0};
  std::uint64_t invalid_json_count{0};
  std::uint64_t unexpected_envelope_count{0};
  std::uint64_t decode_continuity_lost_count{0};
};

struct OrderFeedbackParseResult {
  OrderFeedbackParseStatus status{
      OrderFeedbackParseStatus::kUnexpectedEnvelope};
  std::uint32_t orders_seen{0};
  std::uint32_t events_emitted{0};
  bool continuity_lost{false};
};

enum class OrderFeedbackDiagnosticKind : std::uint8_t {
  kProtocolUpdate,
  kClientOidIgnored,
};

enum class ClientOidIgnoreReason : std::uint8_t {
  kNone,
  kForeignRunNamespace,
  kLegacyClientOid,
};

struct OrderFeedbackDiagnosticRecord {
  std::string_view client_oid;
  std::string_view order_id;
  std::string_view order_status;
  std::string_view cancel_reason;
  OrderFeedbackDiagnosticKind kind{
      OrderFeedbackDiagnosticKind::kProtocolUpdate};
  ClientOidIgnoreReason ignore_reason{ClientOidIgnoreReason::kNone};
  std::uint64_t exchange_message_time_ms{0};
  std::uint64_t created_time_ms{0};
  std::uint64_t updated_time_ms{0};
  std::uint32_t batch_data_index{0};
};

namespace detail {

using ::aquila::exchange::detail::FindSimdjsonField;
using ::aquila::exchange::detail::FindSimdjsonObject;
using ::aquila::exchange::detail::ReadSimdjsonString;

inline constexpr double kOrderFeedbackQuantityEpsilon = 1e-12;

enum class OrderRecordParseOutcome : std::uint8_t {
  kEmitted,
  kIgnored,
  kUnrecoverable,
};

[[nodiscard]] inline bool ParseOrderFeedbackUint64Text(
    std::string_view text, std::uint64_t* output) noexcept {
  assert(output != nullptr);
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const char* const end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return false;
  }
  *output = value;
  return true;
}

[[nodiscard]] inline bool ReadOrderFeedbackUint64(
    simdjson::ondemand::value value, std::uint64_t* output) noexcept {
  assert(output != nullptr);
  std::string_view text;
  if (ReadSimdjsonString(value, &text)) {
    return ParseOrderFeedbackUint64Text(text, output);
  }
  std::uint64_t unsigned_value = 0;
  if (value.get_uint64().get(unsigned_value) == simdjson::SUCCESS) {
    *output = unsigned_value;
    return true;
  }
  std::int64_t signed_value = 0;
  if (value.get_int64().get(signed_value) == simdjson::SUCCESS &&
      signed_value >= 0) {
    *output = static_cast<std::uint64_t>(signed_value);
    return true;
  }
  return false;
}

[[nodiscard]] inline bool ReadOrderFeedbackId(
    simdjson::ondemand::value value, std::uint64_t* output,
    std::string_view* raw_text) noexcept {
  assert(output != nullptr);
  assert(raw_text != nullptr);
  simdjson::ondemand::json_type type;
  if (value.type().get(type) != simdjson::SUCCESS) {
    return false;
  }
  if (type == simdjson::ondemand::json_type::string) {
    return ReadSimdjsonString(value, raw_text) &&
           ParseOrderFeedbackUint64Text(*raw_text, output);
  }
  if (type != simdjson::ondemand::json_type::number) {
    return false;
  }
  std::uint64_t unsigned_value = 0;
  if (value.get_uint64().get(unsigned_value) == simdjson::SUCCESS) {
    *output = unsigned_value;
    return true;
  }
  std::int64_t signed_value = 0;
  if (value.get_int64().get(signed_value) == simdjson::SUCCESS &&
      signed_value >= 0) {
    *output = static_cast<std::uint64_t>(signed_value);
    return true;
  }
  return false;
}

[[nodiscard]] inline bool ParseDoubleText(std::string_view text,
                                          double* output) noexcept {
  assert(output != nullptr);
  if (text.empty()) {
    return false;
  }
  double value = 0.0;
  const char* const end = text.data() + text.size();
  const auto parsed = fast_float::from_chars(text.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value)) {
    return false;
  }
  *output = value;
  return true;
}

[[nodiscard]] inline bool ReadDouble(simdjson::ondemand::value value,
                                     double* output) noexcept {
  assert(output != nullptr);
  std::string_view text;
  if (ReadSimdjsonString(value, &text)) {
    return ParseDoubleText(text, output);
  }
  double number = 0.0;
  if (value.get_double().get(number) != simdjson::SUCCESS ||
      !std::isfinite(number)) {
    return false;
  }
  *output = number;
  return true;
}

[[nodiscard]] inline bool ReadStringField(simdjson::ondemand::object object,
                                          std::string_view name,
                                          std::string_view* output) noexcept {
  simdjson::ondemand::value value;
  return FindSimdjsonField(object, name, &value) &&
         ReadSimdjsonString(value, output);
}

[[nodiscard]] inline bool ReadUint64Field(simdjson::ondemand::object object,
                                          std::string_view name,
                                          std::uint64_t* output) noexcept {
  simdjson::ondemand::value value;
  return FindSimdjsonField(object, name, &value) &&
         ReadOrderFeedbackUint64(value, output);
}

[[nodiscard]] inline bool ReadDoubleField(simdjson::ondemand::object object,
                                          std::string_view name,
                                          double* output) noexcept {
  simdjson::ondemand::value value;
  return FindSimdjsonField(object, name, &value) && ReadDouble(value, output);
}

[[nodiscard]] inline bool MillisecondsToNanoseconds(
    std::uint64_t milliseconds, std::int64_t* output) noexcept {
  assert(output != nullptr);
  constexpr std::uint64_t kScale = 1'000'000;
  if (milliseconds == 0 ||
      milliseconds >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
              kScale) {
    return false;
  }
  *output = static_cast<std::int64_t>(milliseconds * kScale);
  return true;
}

[[nodiscard]] inline OrderFinishReason ParseCancelReason(
    std::string_view cancel_reason) noexcept {
  if (cancel_reason == "normal_cancel") {
    return OrderFinishReason::kManualCancelled;
  }
  if (cancel_reason == "ioc_not_full_cancel") {
    return OrderFinishReason::kImmediateOrCancel;
  }
  if (cancel_reason == "self_trade_cancel" || cancel_reason == "stp_cancel") {
    return OrderFinishReason::kSelfTradePrevention;
  }
  if (cancel_reason == "adl_cancel") {
    return OrderFinishReason::kAutoDeleveraging;
  }
  if (cancel_reason == "burst_cancel" || cancel_reason == "penetrate_cancel") {
    return OrderFinishReason::kLiquidated;
  }
  return OrderFinishReason::kUnknown;
}

struct NoopOrderFeedbackDiagnosticSink {
  void operator()(const OrderFeedbackDiagnosticRecord&) const noexcept {}
};

template <typename EventSink, typename DiagnosticSink>
[[nodiscard]] inline OrderRecordParseOutcome ParseOrderRecord(
    simdjson::ondemand::object order, std::int64_t local_receive_ns,
    std::uint64_t exchange_message_time_ms, std::uint32_t batch_data_index,
    OrderFeedbackParserStats& stats, OrderFeedbackParseResult& result,
    const ClientOidRunNamespace& expected_run_namespace, EventSink& event_sink,
    DiagnosticSink& diagnostic_sink) noexcept {
  constexpr bool kDiagnosticFieldsEnabled =
      !std::is_same_v<std::remove_cvref_t<DiagnosticSink>,
                      NoopOrderFeedbackDiagnosticSink>;
  simdjson::ondemand::value client_oid_value;
  if (!FindSimdjsonField(order, "clientOid", &client_oid_value)) {
    ++stats.unroutable_orders_ignored;
    return OrderRecordParseOutcome::kIgnored;
  }

  std::string_view client_oid;
  simdjson::ondemand::json_type client_oid_type;
  if (client_oid_value.type().get(client_oid_type) != simdjson::SUCCESS) {
    ++stats.validation_errors;
    return OrderRecordParseOutcome::kUnrecoverable;
  }
  if (client_oid_type != simdjson::ondemand::json_type::string) {
    ++stats.unroutable_orders_ignored;
    return OrderRecordParseOutcome::kIgnored;
  }
  if (!ReadSimdjsonString(client_oid_value, &client_oid)) {
    ++stats.validation_errors;
    return OrderRecordParseOutcome::kUnrecoverable;
  }
  if (client_oid.empty()) {
    ++stats.unroutable_orders_ignored;
    return OrderRecordParseOutcome::kIgnored;
  }
  if (client_oid.starts_with("a-")) {
    ++stats.legacy_client_oid_orders_ignored;
    if constexpr (kDiagnosticFieldsEnabled) {
      diagnostic_sink(OrderFeedbackDiagnosticRecord{
          .client_oid = client_oid,
          .kind = OrderFeedbackDiagnosticKind::kClientOidIgnored,
          .ignore_reason = ClientOidIgnoreReason::kLegacyClientOid,
          .exchange_message_time_ms = exchange_message_time_ms,
          .batch_data_index = batch_data_index,
      });
    }
    return OrderRecordParseOutcome::kIgnored;
  }
  if (!client_oid.starts_with("a1-")) {
    ++stats.foreign_orders_ignored;
    return OrderRecordParseOutcome::kIgnored;
  }

  if (client_oid.size() < 3 + kClientOidRunNamespaceSize) {
    ++stats.unroutable_orders_ignored;
    return OrderRecordParseOutcome::kIgnored;
  }
  const std::optional<ClientOidRunNamespace> parsed_run_namespace =
      ClientOidRunNamespace::Parse(
          client_oid.substr(3, kClientOidRunNamespaceSize));
  if (!parsed_run_namespace.has_value()) {
    ++stats.unroutable_orders_ignored;
    return OrderRecordParseOutcome::kIgnored;
  }
  if (*parsed_run_namespace != expected_run_namespace) {
    ++stats.foreign_run_namespace_orders_ignored;
    if constexpr (kDiagnosticFieldsEnabled) {
      diagnostic_sink(OrderFeedbackDiagnosticRecord{
          .client_oid = client_oid,
          .kind = OrderFeedbackDiagnosticKind::kClientOidIgnored,
          .ignore_reason = ClientOidIgnoreReason::kForeignRunNamespace,
          .exchange_message_time_ms = exchange_message_time_ms,
          .batch_data_index = batch_data_index,
      });
    }
    return OrderRecordParseOutcome::kIgnored;
  }

  const ParsedClientOid parsed_client_oid = ClientOidCodec::Parse(client_oid);
  if (!parsed_client_oid.ok ||
      parsed_client_oid.run_namespace != expected_run_namespace) {
    ++stats.validation_errors;
    return OrderRecordParseOutcome::kUnrecoverable;
  }

  std::string_view category;
  std::string_view hold_mode;
  std::string_view margin_mode;
  std::string_view order_status;
  std::string_view order_id;
  std::string_view cancel_reason;
  std::uint64_t exchange_order_id = 0;
  std::uint64_t created_time_ms = 0;
  std::uint64_t updated_time_ms = 0;
  double quantity = 0.0;
  double cumulative_filled_quantity = 0.0;
  double average_price = 0.0;
  if (!ReadStringField(order, "category", &category)) {
    ++stats.validation_errors;
    return OrderRecordParseOutcome::kUnrecoverable;
  }
  if constexpr (kDiagnosticFieldsEnabled) {
    simdjson::ondemand::value order_id_value;
    if (!FindSimdjsonField(order, "orderId", &order_id_value) ||
        !ReadOrderFeedbackId(order_id_value, &exchange_order_id, &order_id)) {
      ++stats.validation_errors;
      return OrderRecordParseOutcome::kUnrecoverable;
    }
  } else if (!ReadUint64Field(order, "orderId", &exchange_order_id)) {
    ++stats.validation_errors;
    return OrderRecordParseOutcome::kUnrecoverable;
  }
  if (!ReadDoubleField(order, "qty", &quantity) ||
      !ReadStringField(order, "holdMode", &hold_mode) ||
      !ReadStringField(order, "marginMode", &margin_mode) ||
      !ReadDoubleField(order, "cumExecQty", &cumulative_filled_quantity) ||
      !ReadDoubleField(order, "avgPrice", &average_price) ||
      !ReadStringField(order, "orderStatus", &order_status) ||
      !ReadUint64Field(order, "updatedTime", &updated_time_ms)) {
    ++stats.validation_errors;
    return OrderRecordParseOutcome::kUnrecoverable;
  }
  if constexpr (kDiagnosticFieldsEnabled) {
    simdjson::ondemand::value created_time_value;
    if (FindSimdjsonField(order, "createdTime", &created_time_value)) {
      (void)ReadOrderFeedbackUint64(created_time_value, &created_time_ms);
    }
  }

  std::int64_t exchange_update_ns = 0;
  if (category != "usdt-futures" || hold_mode != "one_way_mode" ||
      margin_mode != "crossed" || exchange_order_id == 0 || quantity <= 0.0 ||
      cumulative_filled_quantity < 0.0 ||
      cumulative_filled_quantity > quantity + kOrderFeedbackQuantityEpsilon ||
      !MillisecondsToNanoseconds(updated_time_ms, &exchange_update_ns)) {
    ++stats.validation_errors;
    return OrderRecordParseOutcome::kUnrecoverable;
  }

  if (cumulative_filled_quantity > 0.0 && average_price <= 0.0) {
    ++stats.validation_errors;
    return OrderRecordParseOutcome::kUnrecoverable;
  }

  const double left_quantity =
      std::max(0.0, quantity - cumulative_filled_quantity);
  OrderFeedbackEvent event{};
  event.local_order_id = parsed_client_oid.local_order_id;
  event.exchange_order_id = exchange_order_id;
  event.cumulative_filled_quantity = cumulative_filled_quantity;
  event.left_quantity = left_quantity;
  event.fill_price = average_price;
  event.role = OrderRole::kNone;
  event.exchange_update_ns = exchange_update_ns;
  event.local_receive_ns = local_receive_ns;

  if (order_status == "new") {
    if (std::abs(cumulative_filled_quantity) > kOrderFeedbackQuantityEpsilon) {
      ++stats.validation_errors;
      return OrderRecordParseOutcome::kUnrecoverable;
    }
    event.kind = OrderFeedbackKind::kAccepted;
  } else if (order_status == "partially_filled") {
    if (cumulative_filled_quantity <= 0.0 ||
        cumulative_filled_quantity + kOrderFeedbackQuantityEpsilon >=
            quantity) {
      ++stats.validation_errors;
      return OrderRecordParseOutcome::kUnrecoverable;
    }
    event.kind = OrderFeedbackKind::kPartialFilled;
  } else if (order_status == "filled") {
    if (std::abs(cumulative_filled_quantity - quantity) >
        kOrderFeedbackQuantityEpsilon) {
      ++stats.validation_errors;
      return OrderRecordParseOutcome::kUnrecoverable;
    }
    event.kind = OrderFeedbackKind::kFilled;
  } else if (order_status == "cancelled" || order_status == "canceled") {
    if (order_status == "canceled") {
      ++stats.legacy_canceled_statuses;
    }
    (void)ReadStringField(order, "cancelReason", &cancel_reason);
    event.kind = OrderFeedbackKind::kCancelled;
    event.cancelled_quantity = left_quantity;
    event.finish_reason = ParseCancelReason(cancel_reason);
  } else {
    ++stats.validation_errors;
    return OrderRecordParseOutcome::kUnrecoverable;
  }

  if constexpr (kDiagnosticFieldsEnabled) {
    diagnostic_sink(OrderFeedbackDiagnosticRecord{
        .client_oid = client_oid,
        .order_id = order_id,
        .order_status = order_status,
        .cancel_reason = cancel_reason,
        .exchange_message_time_ms = exchange_message_time_ms,
        .created_time_ms = created_time_ms,
        .updated_time_ms = updated_time_ms,
        .batch_data_index = batch_data_index,
    });
  }
  event_sink(event);
  ++stats.events_emitted;
  ++result.events_emitted;
  return OrderRecordParseOutcome::kEmitted;
}

template <typename EventSink, typename DiagnosticSink>
[[nodiscard]] inline OrderFeedbackParseResult ParseDocument(
    simdjson::ondemand::document document, std::int64_t local_receive_ns,
    OrderFeedbackParserStats& stats, EventSink& event_sink,
    DiagnosticSink& diagnostic_sink,
    const ClientOidRunNamespace& expected_run_namespace) noexcept {
  constexpr bool kDiagnosticFieldsEnabled =
      !std::is_same_v<std::remove_cvref_t<DiagnosticSink>,
                      NoopOrderFeedbackDiagnosticSink>;
  OrderFeedbackParseResult result{};
  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    ++stats.unexpected_envelope_count;
    result.status = OrderFeedbackParseStatus::kUnexpectedEnvelope;
    result.continuity_lost = true;
    return result;
  }

  std::string_view action;
  simdjson::ondemand::value action_value;
  if (!FindSimdjsonField(root, "action", &action_value)) {
    simdjson::ondemand::value event_value;
    std::string_view event;
    if (FindSimdjsonField(root, "event", &event_value) &&
        ReadSimdjsonString(event_value, &event)) {
      result.status = OrderFeedbackParseStatus::kControlMessage;
      result.continuity_lost = false;
      return result;
    }
    ++stats.unexpected_envelope_count;
    result.status = OrderFeedbackParseStatus::kUnexpectedEnvelope;
    result.continuity_lost = true;
    return result;
  }
  simdjson::ondemand::object arg;
  std::string_view inst_type;
  std::string_view topic;
  if (!ReadSimdjsonString(action_value, &action) ||
      !FindSimdjsonObject(root, "arg", &arg) ||
      !ReadStringField(arg, "instType", &inst_type) || inst_type != "UTA" ||
      !ReadStringField(arg, "topic", &topic)) {
    ++stats.unexpected_envelope_count;
    result.status = OrderFeedbackParseStatus::kUnexpectedEnvelope;
    result.continuity_lost = true;
    return result;
  }
  if (action == "update" && topic == "fast-fill") {
    result.status = OrderFeedbackParseStatus::kFastFillMessage;
    result.continuity_lost = false;
    return result;
  }

  if (action != "snapshot" || topic != "order") {
    ++stats.unexpected_envelope_count;
    result.status = OrderFeedbackParseStatus::kUnexpectedEnvelope;
    result.continuity_lost = true;
    return result;
  }

  std::uint64_t exchange_message_time_ms = 0;
  if constexpr (kDiagnosticFieldsEnabled) {
    simdjson::ondemand::value exchange_message_time_value;
    if (FindSimdjsonField(root, "ts", &exchange_message_time_value)) {
      (void)ReadOrderFeedbackUint64(exchange_message_time_value,
                                    &exchange_message_time_ms);
    }
  }

  simdjson::ondemand::value data_value;
  simdjson::ondemand::array data;
  if (!FindSimdjsonField(root, "data", &data_value) ||
      data_value.get_array().get(data) != simdjson::SUCCESS) {
    ++stats.unexpected_envelope_count;
    result.status = OrderFeedbackParseStatus::kUnexpectedEnvelope;
    result.continuity_lost = true;
    return result;
  }

  ++stats.order_envelopes;
  std::uint32_t batch_data_index = 0;
  for (simdjson::simdjson_result<simdjson::ondemand::value> element : data) {
    simdjson::ondemand::object order;
    if (element.get_object().get(order) != simdjson::SUCCESS) {
      ++stats.validation_errors;
      ++stats.decode_continuity_lost_count;
      result.status = OrderFeedbackParseStatus::kDecodeUnrecoverable;
      result.continuity_lost = true;
      return result;
    }
    ++stats.orders_seen;
    ++result.orders_seen;
    if (ParseOrderRecord(order, local_receive_ns, exchange_message_time_ms,
                         batch_data_index, stats, result,
                         expected_run_namespace, event_sink, diagnostic_sink) ==
        OrderRecordParseOutcome::kUnrecoverable) {
      ++stats.decode_continuity_lost_count;
      result.status = OrderFeedbackParseStatus::kDecodeUnrecoverable;
      result.continuity_lost = true;
      return result;
    }
    ++batch_data_index;
  }

  result.status = OrderFeedbackParseStatus::kOk;
  ++stats.messages_parsed;
  return result;
}

[[nodiscard]] inline OrderFeedbackParseResult ClassifyDeferredJsonError(
    std::string_view payload, OrderFeedbackParseResult result,
    OrderFeedbackParserStats& stats) noexcept {
  if (result.status == OrderFeedbackParseStatus::kOk) {
    return result;
  }

  simdjson::padded_string padded(payload);
  simdjson::dom::parser validator;
  simdjson::dom::element element;
  if (validator.parse(padded).get(element) == simdjson::SUCCESS) {
    return result;
  }

  if (result.status == OrderFeedbackParseStatus::kUnexpectedEnvelope &&
      stats.unexpected_envelope_count != 0) {
    --stats.unexpected_envelope_count;
  } else if (result.status == OrderFeedbackParseStatus::kDecodeUnrecoverable &&
             stats.decode_continuity_lost_count != 0) {
    --stats.decode_continuity_lost_count;
    if (stats.validation_errors != 0) {
      --stats.validation_errors;
    }
  }
  ++stats.invalid_json_count;
  result.status = OrderFeedbackParseStatus::kInvalidJson;
  result.continuity_lost = true;
  return result;
}

[[nodiscard]] inline OrderFeedbackParseResult FinalizeParseResult(
    std::string_view payload, OrderFeedbackParseResult result,
    OrderFeedbackParserStats& stats) noexcept {
  result = ClassifyDeferredJsonError(payload, result, stats);
  if (result.status == OrderFeedbackParseStatus::kControlMessage ||
      result.status == OrderFeedbackParseStatus::kFastFillMessage) {
    assert(stats.messages_seen != 0);
    --stats.messages_seen;
  }
  return result;
}

}  // namespace detail

template <typename EventSink, typename DiagnosticSink>
[[nodiscard]] inline OrderFeedbackParseResult ParseBitgetOrderFeedbackMessage(
    std::string_view payload, std::uint32_t readable_tail_bytes,
    std::int64_t local_receive_ns, simdjson::ondemand::parser& parser,
    OrderFeedbackParserStats& stats,
    const ClientOidRunNamespace& expected_run_namespace, EventSink&& event_sink,
    DiagnosticSink&& diagnostic_sink) noexcept {
  ++stats.messages_seen;
  if (payload.empty()) {
    ++stats.invalid_json_count;
    return {.status = OrderFeedbackParseStatus::kInvalidJson,
            .continuity_lost = true};
  }

  simdjson::ondemand::document document;
  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    const simdjson::padded_string_view view(
        payload.data(), payload.size(), payload.size() + readable_tail_bytes);
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      ++stats.invalid_json_count;
      return {.status = OrderFeedbackParseStatus::kInvalidJson,
              .continuity_lost = true};
    }
    return detail::FinalizeParseResult(
        payload,
        detail::ParseDocument(std::move(document), local_receive_ns, stats,
                              event_sink, diagnostic_sink,
                              expected_run_namespace),
        stats);
  }

  simdjson::padded_string padded(payload);
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    ++stats.invalid_json_count;
    return {.status = OrderFeedbackParseStatus::kInvalidJson,
            .continuity_lost = true};
  }
  return detail::FinalizeParseResult(
      payload,
      detail::ParseDocument(std::move(document), local_receive_ns, stats,
                            event_sink, diagnostic_sink,
                            expected_run_namespace),
      stats);
}

template <typename EventSink>
[[nodiscard]] inline OrderFeedbackParseResult ParseBitgetOrderFeedbackMessage(
    std::string_view payload, std::uint32_t readable_tail_bytes,
    std::int64_t local_receive_ns, simdjson::ondemand::parser& parser,
    OrderFeedbackParserStats& stats,
    const ClientOidRunNamespace& expected_run_namespace,
    EventSink&& event_sink) noexcept {
  detail::NoopOrderFeedbackDiagnosticSink diagnostic_sink;
  return ParseBitgetOrderFeedbackMessage(
      payload, readable_tail_bytes, local_receive_ns, parser, stats,
      expected_run_namespace, std::forward<EventSink>(event_sink),
      diagnostic_sink);
}

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ORDER_FEEDBACK_PARSER_H_
