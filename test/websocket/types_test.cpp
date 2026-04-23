#include "core/websocket/message_view.h"
#include "core/websocket/runtime_policy.h"
#include "core/websocket/types.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace ws = aquila::websocket;

namespace {

ws::DeliveryResult HandleMessage(void* context,
                                 const ws::MessageView& view) noexcept {
  auto* counter = static_cast<size_t*>(context);
  *counter += view.payload.size();
  return view.payload.empty() ? ws::DeliveryResult::kBackpressured
                              : ws::DeliveryResult::kAccepted;
}

template <auto Value>
constexpr bool kEnumValueExists = true;

}  // namespace

int main() {
  static_assert(kEnumValueExists<ws::ConnectionPhase::kActive>);
  static_assert(kEnumValueExists<ws::SchedulingPolicy::kFifo>);

  const ws::ConnectionConfig config;
  if (!config.enable_tls || !config.runtime_policy.lock_memory ||
      !config.runtime_policy.active_spin) {
    return 1;
  }

  std::byte bytes[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  size_t total_bytes = 0;
  const ws::MessageConsumer consumer{.context = &total_bytes,
                                     .handler = &HandleMessage};
  const ws::MessageView view{
      .kind = ws::PayloadKind::kBinary,
      .payload = std::span<const std::byte>(bytes),
      .sequence = 42,
      .fin = true,
  };

  if (consumer.Handle(view) != ws::DeliveryResult::kAccepted) {
    return 1;
  }
  if (total_bytes != std::size(bytes)) {
    return 1;
  }

  return 0;
}
