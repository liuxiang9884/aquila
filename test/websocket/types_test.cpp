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
  if (config.host != "fx-ws.gateio.ws" || config.service != "443" ||
      config.target != "/v4/ws/usdt") {
    return 1;
  }
  if (config.read_buffer_bytes != (size_t{1} << 20) ||
      config.frame_buffer_bytes != (size_t{1} << 20) ||
      config.prepared_write_slots != 2048 ||
      config.prepared_write_bytes != 4096) {
    return 1;
  }
  if (config.heartbeat_interval_ms != 5000 ||
      config.heartbeat_timeout_ms != 15000) {
    return 1;
  }

  const ws::RuntimePolicy runtime_policy = config.runtime_policy;
  if (runtime_policy.affinity_mode != ws::AffinityMode::kRequired ||
      runtime_policy.io_cpu_id != -1 ||
      runtime_policy.scheduling_policy != ws::SchedulingPolicy::kOther ||
      runtime_policy.scheduling_priority != 0 ||
      !runtime_policy.prefault_stack) {
    return 1;
  }

  std::byte bytes[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  size_t total_bytes = 0;
  const ws::MessageConsumer consumer{.context = &total_bytes,
                                     .handler = &HandleMessage};
  const ws::MessageConsumer null_consumer{};
  const ws::MessageView view{
      .kind = ws::PayloadKind::kBinary,
      .payload = std::span<const std::byte>(bytes),
      .sequence = 42,
      .fin = true,
  };
  const ws::MessageView empty_view{
      .kind = ws::PayloadKind::kBinary,
      .payload = {},
      .sequence = 43,
      .fin = true,
  };

  if (null_consumer.Handle(view) != ws::DeliveryResult::kFatal) {
    return 1;
  }

  if (consumer.Handle(view) != ws::DeliveryResult::kAccepted) {
    return 1;
  }
  if (total_bytes != std::size(bytes)) {
    return 1;
  }
  if (consumer.Handle(empty_view) != ws::DeliveryResult::kBackpressured) {
    return 1;
  }
  if (total_bytes != std::size(bytes)) {
    return 1;
  }

  return 0;
}
