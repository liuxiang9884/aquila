#include "core/websocket/message_view.h"
#include "core/websocket/runtime_policy.h"
#include "core/websocket/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

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
  static_assert(
      std::is_same_v<std::underlying_type_t<ws::ConnectionPhase>, std::uint8_t>);
  static_assert(
      std::is_same_v<std::underlying_type_t<ws::ConnectionError>, std::uint8_t>);
  static_assert(
      std::is_same_v<std::underlying_type_t<ws::PayloadKind>, std::uint8_t>);
  static_assert(
      std::is_same_v<std::underlying_type_t<ws::DeliveryResult>, std::uint8_t>);
  static_assert(
      std::is_same_v<std::underlying_type_t<ws::SendStatus>, std::uint8_t>);
  static_assert(
      std::is_same_v<std::underlying_type_t<ws::AffinityMode>, std::uint8_t>);
  static_assert(std::is_same_v<std::underlying_type_t<ws::SchedulingPolicy>,
                               std::uint8_t>);
  static_assert(std::is_same_v<decltype(ws::ConnectionConfig{}.host),
                               std::string>);
  static_assert(std::is_same_v<decltype(ws::ConnectionConfig{}.service),
                               std::string>);
  static_assert(std::is_same_v<decltype(ws::ConnectionConfig{}.target),
                               std::string>);
  static_assert(std::is_same_v<decltype(ws::RuntimePolicy{}
                                            .spin_iterations_before_clock_check),
                               std::uint32_t>);

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
