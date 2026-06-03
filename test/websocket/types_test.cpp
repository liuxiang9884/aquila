#include "core/websocket/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "core/websocket/message_view.h"
#include "core/websocket/runtime_policy.h"

namespace ws = aquila::websocket;

namespace {

ws::DeliveryResult HandleMessage(void* context,
                                 const ws::MessageView& view) noexcept {
  auto* counter = static_cast<size_t*>(context);
  *counter += view.payload.size();
  return view.payload.empty() ? ws::DeliveryResult::kBackpressured
                              : ws::DeliveryResult::kAccepted;
}

struct TypedMessageHandler {
  size_t bytes{0};

  ws::DeliveryResult Handle(const ws::MessageView& view) noexcept {
    bytes += view.payload.size();
    return ws::DeliveryResult::kAccepted;
  }
};

struct SmallPreparedWriteOptions : ws::DefaultWebSocketOptions {
  static constexpr ws::ClockSource kClockSource =
      ws::ClockSource::kMonotonicCoarse;
  static constexpr size_t kPreparedWriteSlots = 7;
  static constexpr size_t kPreparedWriteBytes = 128;
};

template <auto Value>
constexpr bool kEnumValueExists = true;

static_assert(kEnumValueExists<ws::ConnectionPhase::kActive>);
static_assert(kEnumValueExists<ws::ConnectionPhase::kDegraded>);
static_assert(kEnumValueExists<ws::ClockSource::kSteady>);
static_assert(kEnumValueExists<ws::ClockSource::kMonotonic>);
static_assert(kEnumValueExists<ws::ClockSource::kMonotonicCoarse>);
static_assert(kEnumValueExists<ws::ClockSource::kRealtime>);
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
static_assert(
    std::is_same_v<std::underlying_type_t<ws::SchedulingPolicy>, std::uint8_t>);
static_assert(
    std::is_same_v<decltype(ws::ConnectionConfig{}.host), std::string>);
static_assert(
    std::is_same_v<decltype(ws::ConnectionConfig{}.connect_ip), std::string>);
static_assert(
    std::is_same_v<decltype(ws::ConnectionConfig{}.port), std::string>);
static_assert(
    std::is_same_v<decltype(ws::ConnectionConfig{}.target), std::string>);
static_assert(std::is_same_v<decltype(ws::ConnectionConfig{}.extra_headers),
                             std::vector<ws::HttpHeader>>);
static_assert(std::is_same_v<
              decltype(ws::RuntimePolicy{}.spin_iterations_before_clock_check),
              std::uint32_t>);

}  // namespace

TEST(WebsocketTypesTest, ExposesExpectedDefaultsAndHandlers) {
  const ws::ConnectionConfig config;
  EXPECT_TRUE(config.enable_tls);
  EXPECT_TRUE(config.runtime_policy.lock_memory);
  EXPECT_TRUE(config.runtime_policy.active_spin);
  EXPECT_EQ(config.host, "fx-ws.gateio.ws");
  EXPECT_TRUE(config.connect_ip.empty());
  EXPECT_EQ(config.port, "443");
  EXPECT_EQ(config.target, "/v4/ws/usdt");
  EXPECT_TRUE(config.extra_headers.empty());
  EXPECT_EQ(config.read_buffer_bytes, (size_t{1} << 20));
  EXPECT_EQ(config.frame_buffer_bytes, (size_t{1} << 20));
  EXPECT_EQ(config.max_frame_payload_bytes, (size_t{1} << 20));
  EXPECT_EQ(config.max_reads_per_drive, 1U);
  EXPECT_FALSE(config.read_until_would_block);
  EXPECT_EQ(config.prepared_write_slots,
            ws::DefaultWebSocketOptions::kPreparedWriteSlots);
  EXPECT_EQ(config.prepared_write_bytes,
            ws::DefaultWebSocketOptions::kPreparedWriteBytes);
  EXPECT_EQ(config.max_business_writes_per_drive, 1U);
  EXPECT_EQ(config.heartbeat_interval_ms, 5000U);
  EXPECT_EQ(config.heartbeat_timeout_ms, 15000U);
  EXPECT_EQ(config.degraded.high_watermark_percent, 80U);
  EXPECT_EQ(config.degraded.high_watermark_hold_ticks, 8U);
  EXPECT_EQ(config.degraded.recover_ticks, 16U);
  EXPECT_EQ(config.degraded.backpressure_drops_per_second, 10U);
  EXPECT_EQ(config.degraded.awaiting_pong_timeout_ms, 3000U);
  EXPECT_EQ(config.degraded.frame_codec_capacity_events_per_second, 1U);
  EXPECT_EQ(config.degraded.evaluation_interval_iterations, 0U);

  const ws::RuntimePolicy runtime_policy = config.runtime_policy;
  EXPECT_EQ(runtime_policy.affinity_mode, ws::AffinityMode::kRequired);
  EXPECT_EQ(runtime_policy.io_cpu_id, -1);
  EXPECT_EQ(runtime_policy.scheduling_policy, ws::SchedulingPolicy::kOther);
  EXPECT_EQ(runtime_policy.scheduling_priority, 0);
  EXPECT_TRUE(runtime_policy.prefault_stack);
  EXPECT_EQ(runtime_policy.clock_source, ws::ClockSource::kSteady);

  std::byte bytes[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  size_t total_bytes = 0;
  const ws::MessageCallback consumer{.context = &total_bytes,
                                     .handler = &HandleMessage};
  const ws::MessageCallback null_consumer{};
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

  EXPECT_EQ(null_consumer.Handle(view), ws::DeliveryResult::kFatal);
  EXPECT_EQ(consumer.Handle(view), ws::DeliveryResult::kAccepted);
  EXPECT_EQ(total_bytes, std::size(bytes));
  EXPECT_EQ(consumer.Handle(empty_view), ws::DeliveryResult::kBackpressured);
  EXPECT_EQ(total_bytes, std::size(bytes));
}

TEST(WebsocketTypesTest,
     BuildsPreparedWriteDefaultsFromOptionsAndAllowsRuntimeOverride) {
  ws::ConnectionConfig config =
      ws::MakeConnectionConfig<SmallPreparedWriteOptions>();

  EXPECT_EQ(config.prepared_write_slots,
            SmallPreparedWriteOptions::kPreparedWriteSlots);
  EXPECT_EQ(config.prepared_write_bytes,
            SmallPreparedWriteOptions::kPreparedWriteBytes);
  EXPECT_EQ(config.runtime_policy.clock_source,
            SmallPreparedWriteOptions::kClockSource);

  config.prepared_write_slots = 3;
  config.prepared_write_bytes = 256;

  EXPECT_EQ(config.prepared_write_slots, 3U);
  EXPECT_EQ(config.prepared_write_bytes, 256U);
}

TEST(WebsocketTypesTest, MessageHandlerRefForwardsToTypedHandler) {
  std::byte bytes[] = {std::byte{0x01}, std::byte{0x02}};
  TypedMessageHandler handler;
  const auto handler_ref = ws::MakeMessageHandler(handler);
  const ws::MessageView view{
      .kind = ws::PayloadKind::kBinary,
      .payload = std::span<const std::byte>(bytes),
      .sequence = 44,
      .fin = true,
  };

  EXPECT_EQ(handler_ref.Handle(view), ws::DeliveryResult::kAccepted);
  EXPECT_EQ(handler.bytes, std::size(bytes));
}
