#ifndef AQUILA_CORE_WEBSOCKET_MESSAGE_VIEW_H_
#define AQUILA_CORE_WEBSOCKET_MESSAGE_VIEW_H_

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/common/data_session_diagnostic_level.h"
#include "core/websocket/types.h"

namespace aquila::websocket {

struct MessageView {
  PayloadKind kind{PayloadKind::kBinary};
  std::span<const std::byte> payload{};
  std::uint64_t sequence{0};
  bool fin{true};
  // Bytes after payload that are mapped and readable while this view is valid.
  // They are not payload bytes and may contain arbitrary ring contents.
  std::uint32_t readable_tail_bytes{0};
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 2
  std::int64_t drive_read_enter_ns{0};
  std::int64_t read_return_ns{0};
  std::int64_t handler_entry_ns{0};
  std::int32_t read_bytes{0};
#endif
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 4
  bool kernel_rx_available{false};
  std::int64_t kernel_rx_ns{0};
#endif
};

using MessageHandler = DeliveryResult (*)(void* context,
                                          const MessageView& view) noexcept;

struct MessageCallback {
  void* context{nullptr};
  MessageHandler handler{nullptr};

  DeliveryResult Handle(const MessageView& view) const noexcept {
    return handler == nullptr ? DeliveryResult::kFatal : handler(context, view);
  }
};

template <typename MessageHandlerT>
struct MessageHandlerRef {
  MessageHandlerT* handler{nullptr};

  DeliveryResult Handle(const MessageView& view) const noexcept {
    return handler == nullptr ? DeliveryResult::kFatal : handler->Handle(view);
  }
};

template <typename MessageHandlerT>
MessageHandlerRef<MessageHandlerT> MakeMessageHandler(
    MessageHandlerT& handler) noexcept {
  return {.handler = &handler};
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_MESSAGE_VIEW_H_
