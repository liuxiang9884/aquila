#ifndef AQUILA_EXCHANGE_GATE_SBE_MESSAGE_DISPATCHER_H_
#define AQUILA_EXCHANGE_GATE_SBE_MESSAGE_DISPATCHER_H_

#include <cstdint>
#include <string_view>

#include <sbepp/sbepp.hpp>

#include "exchange/gate/sbe/generated/gate/gate.hpp"
#include "exchange/gate/sbe/message_header.h"

namespace aquila::gate {

inline constexpr std::uint16_t kGateSbeSchemaId =
    ::sbepp::schema_traits<::gate::schema>::id();
inline constexpr std::uint16_t kGateSbeSchemaVersion =
    ::sbepp::schema_traits<::gate::schema>::version();

inline constexpr std::uint16_t kGateSbeBookTickerTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::bbo>::id();
inline constexpr std::uint16_t kGateSbePublicTradeTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::publicTrade>::id();
inline constexpr std::uint16_t kGateSbeObuTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::obu>::id();
inline constexpr std::uint16_t kGateSbeOrderBookTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::orderBook>::id();
inline constexpr std::uint16_t kGateSbeOrderBookUpdateTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::orderBookUpdate>::id();
inline constexpr std::uint16_t kGateSbeUserTradeTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::userTrade>::id();
inline constexpr std::uint16_t kGateSbePositionTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::position>::id();
inline constexpr std::uint16_t kGateSbeCandlestickTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::candlestick>::id();
inline constexpr std::uint16_t kGateSbeFuturesTickerTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::futuresTicker>::id();
inline constexpr std::uint16_t kGateSbeOrdersTemplateId =
    ::sbepp::message_traits<::gate::schema::messages::orders>::id();

enum class GateSbeMessageType : std::uint8_t {
  kUnknown = 0,
  kBookTicker,
  kPublicTrade,
  kObu,
  kOrderBook,
  kOrderBookUpdate,
  kUserTrade,
  kPosition,
  kCandlestick,
  kFuturesTicker,
  kOrders,
};

enum class SbeDispatchStatus : std::uint8_t {
  kReady = 0,
  kNeedMore,
  kUnsupportedSchema,
  kUnsupportedSchemaVersion,
  kUnsupportedTemplate,
};

struct SbeDispatchResult {
  SbeDispatchStatus status;
  SbeMessageHeader header;
  GateSbeMessageType message_type;
};

inline GateSbeMessageType GateSbeMessageTypeFromTemplateId(
    std::uint16_t template_id) noexcept {
  switch (template_id) {
    case kGateSbeBookTickerTemplateId:
      return GateSbeMessageType::kBookTicker;
    case kGateSbePublicTradeTemplateId:
      return GateSbeMessageType::kPublicTrade;
    case kGateSbeObuTemplateId:
      return GateSbeMessageType::kObu;
    case kGateSbeOrderBookTemplateId:
      return GateSbeMessageType::kOrderBook;
    case kGateSbeOrderBookUpdateTemplateId:
      return GateSbeMessageType::kOrderBookUpdate;
    case kGateSbeUserTradeTemplateId:
      return GateSbeMessageType::kUserTrade;
    case kGateSbePositionTemplateId:
      return GateSbeMessageType::kPosition;
    case kGateSbeCandlestickTemplateId:
      return GateSbeMessageType::kCandlestick;
    case kGateSbeFuturesTickerTemplateId:
      return GateSbeMessageType::kFuturesTicker;
    case kGateSbeOrdersTemplateId:
      return GateSbeMessageType::kOrders;
    default:
      return GateSbeMessageType::kUnknown;
  }
}

inline SbeDispatchResult DispatchGateSbeMessage(
    const SbeMessageHeader& header) noexcept {
  if (header.schema_id != kGateSbeSchemaId) {
    return {.status = SbeDispatchStatus::kUnsupportedSchema,
            .header = header,
            .message_type = GateSbeMessageType::kUnknown};
  }

  if (header.version != kGateSbeSchemaVersion) {
    return {.status = SbeDispatchStatus::kUnsupportedSchemaVersion,
            .header = header,
            .message_type = GateSbeMessageType::kUnknown};
  }

  const GateSbeMessageType message_type =
      GateSbeMessageTypeFromTemplateId(header.template_id);
  if (message_type == GateSbeMessageType::kUnknown) {
    return {.status = SbeDispatchStatus::kUnsupportedTemplate,
            .header = header,
            .message_type = message_type};
  }

  return {.status = SbeDispatchStatus::kReady,
          .header = header,
          .message_type = message_type};
}

inline SbeDispatchResult DispatchSbeMessage(std::string_view payload) noexcept {
  SbeMessageHeader header{};
  if (!ParseSbeMessageHeader(payload, &header)) {
    return {.status = SbeDispatchStatus::kNeedMore,
            .header = header,
            .message_type = GateSbeMessageType::kUnknown};
  }

  switch (header.schema_id) {
    case kGateSbeSchemaId:
      return DispatchGateSbeMessage(header);
    default:
      return {.status = SbeDispatchStatus::kUnsupportedSchema,
              .header = header,
              .message_type = GateSbeMessageType::kUnknown};
  }
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_SBE_MESSAGE_DISPATCHER_H_
