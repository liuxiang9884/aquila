#ifndef AQUILA_CORE_WEBSOCKET_MESSAGE_VIEW_H_
#define AQUILA_CORE_WEBSOCKET_MESSAGE_VIEW_H_

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/websocket/types.h"

struct MessageView {
  PayloadKind kind{PayloadKind::kBinary};
  std::span<const std::byte> payload{};
  std::uint64_t sequence{0};
  bool fin{true};
};

using MessageHandler = DeliveryResult (*)(void* context,
                                          const MessageView& view) noexcept;

struct MessageConsumer {
  void* context{nullptr};
  MessageHandler handler{nullptr};

  DeliveryResult Handle(const MessageView& view) const noexcept {
    return handler == nullptr ? DeliveryResult::kFatal : handler(context, view);
  }
};

#endif  // AQUILA_CORE_WEBSOCKET_MESSAGE_VIEW_H_
