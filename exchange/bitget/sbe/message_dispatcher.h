#ifndef AQUILA_EXCHANGE_BITGET_SBE_MESSAGE_DISPATCHER_H_
#define AQUILA_EXCHANGE_BITGET_SBE_MESSAGE_DISPATCHER_H_

#include <cstdint>
#include <string_view>

#include "exchange/bitget/sbe/message_header.h"

namespace aquila::bitget {

inline constexpr std::uint16_t kBitgetSbeSchemaId = 1;
inline constexpr std::uint16_t kBitgetSbeSchemaVersion = 2;
inline constexpr std::uint16_t kBitgetSbeBookTickerTemplateId = 1002;

enum class BitgetSbeMessageType : std::uint8_t {
  kUnknown = 0,
  kBookTicker,
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
  BitgetSbeMessageType message_type;
};

inline BitgetSbeMessageType BitgetSbeMessageTypeFromTemplateId(
    std::uint16_t template_id) noexcept {
  switch (template_id) {
    case kBitgetSbeBookTickerTemplateId:
      return BitgetSbeMessageType::kBookTicker;
    default:
      return BitgetSbeMessageType::kUnknown;
  }
}

inline SbeDispatchResult DispatchBitgetSbeMessage(
    const SbeMessageHeader& header) noexcept {
  if (header.schema_id != kBitgetSbeSchemaId) {
    return {.status = SbeDispatchStatus::kUnsupportedSchema,
            .header = header,
            .message_type = BitgetSbeMessageType::kUnknown};
  }

  if (header.version != kBitgetSbeSchemaVersion) {
    return {.status = SbeDispatchStatus::kUnsupportedSchemaVersion,
            .header = header,
            .message_type = BitgetSbeMessageType::kUnknown};
  }

  const BitgetSbeMessageType message_type =
      BitgetSbeMessageTypeFromTemplateId(header.template_id);
  if (message_type == BitgetSbeMessageType::kUnknown) {
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
            .message_type = BitgetSbeMessageType::kUnknown};
  }

  switch (header.schema_id) {
    case kBitgetSbeSchemaId:
      return DispatchBitgetSbeMessage(header);
    default:
      return {.status = SbeDispatchStatus::kUnsupportedSchema,
              .header = header,
              .message_type = BitgetSbeMessageType::kUnknown};
  }
}

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_SBE_MESSAGE_DISPATCHER_H_
