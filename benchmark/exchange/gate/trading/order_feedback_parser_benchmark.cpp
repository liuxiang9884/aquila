#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

#include <benchmark/benchmark.h>
#include <sbepp/sbepp.hpp>

#include "core/trading/order_feedback_shm.h"
#include "core/trading/order_id.h"
#include "core/websocket/message_view.h"
#include "exchange/gate/sbe/generated/gate/messages/orders.hpp"
#include "exchange/gate/sbe/generated/gate/types/Event.hpp"
#include "exchange/gate/sbe/message_dispatcher.h"
#include "exchange/gate/trading/order_feedback_parser.h"
#include "exchange/gate/trading/order_feedback_session.h"

namespace aquila::gate {
namespace {

constexpr std::uint8_t kStrategyId = 3;
constexpr std::uint64_t kLocalOrderId =
    LocalOrderIdCodec::Encode(kStrategyId, 42);
constexpr std::uint64_t kExchangeOrderId = 36'028'827'892'199'865ULL;
constexpr std::int64_t kLocalReceiveNs = 1'770'000'000'001'222'000;
constexpr std::int64_t kUpdateTimeUs = 1'770'000'000'001'111;

struct OrderPayloadFields {
  std::string_view finish_as{"_new"};
  std::string_view role{};
  std::int64_t left_mantissa{4};
  std::int64_t size_mantissa{10};
};

template <typename T, std::size_t N>
void WriteLittleEndian(std::array<char, N>& buffer, std::size_t offset,
                       T value) noexcept {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

template <std::size_t N>
void AppendVarString(std::array<char, N>& buffer, std::size_t* offset,
                     std::string_view value) noexcept {
  buffer[(*offset)++] = static_cast<char>(value.size());
  std::memcpy(buffer.data() + *offset, value.data(), value.size());
  *offset += value.size();
}

template <std::size_t N>
std::string_view BuildOrdersPayload(std::array<char, N>* buffer,
                                    const OrderPayloadFields& fields) noexcept {
  static constexpr std::uint16_t kMessageBlockLength =
      ::sbepp::message_traits<::gate::schema::messages::orders>::block_length();
  static constexpr std::uint16_t kResultBlockLength = ::sbepp::group_traits<
      ::gate::schema::messages::orders::result>::block_length();
  static_assert(kMessageBlockLength == 9);
  static_assert(kResultBlockLength == 156);

  std::size_t offset = 0;
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kMessageBlockLength);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kGateSbeOrdersTemplateId);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kGateSbeSchemaId);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, kGateSbeSchemaVersion);
  offset += sizeof(std::uint16_t);

  WriteLittleEndian<std::int64_t>(*buffer, offset, kUpdateTimeUs);
  offset += sizeof(std::int64_t);
  WriteLittleEndian<std::int8_t>(
      *buffer, offset, static_cast<std::int8_t>(::gate::types::Event::Update));
  offset += sizeof(std::int8_t);

  WriteLittleEndian<std::uint16_t>(*buffer, offset, kResultBlockLength);
  offset += sizeof(std::uint16_t);
  WriteLittleEndian<std::uint16_t>(*buffer, offset, 1);
  offset += sizeof(std::uint16_t);

  const std::size_t entry_offset = offset;
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 0,
                                  kUpdateTimeUs - 10);
  WriteLittleEndian<std::uint64_t>(*buffer, entry_offset + 8, kExchangeOrderId);
  WriteLittleEndian<std::int8_t>(*buffer, entry_offset + 16, 0);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 25,
                                  fields.left_mantissa);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 33,
                                  fields.size_mantissa);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 57, 0);
  WriteLittleEndian<std::int8_t>(*buffer, entry_offset + 65, 1);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 66, kUpdateTimeUs);
  WriteLittleEndian<std::int8_t>(*buffer, entry_offset + 87, -2);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 88, 6'501'250);
  WriteLittleEndian<std::int64_t>(*buffer, entry_offset + 96, 6'501'250);
  offset += kResultBlockLength;

  AppendVarString(*buffer, &offset, "BTC_USDT");
  AppendVarString(*buffer, &offset, fields.role);
  AppendVarString(*buffer, &offset, "t-216172782113783850");
  AppendVarString(*buffer, &offset, "gtc");
  AppendVarString(*buffer, &offset, fields.finish_as);
  AppendVarString(*buffer, &offset, "open");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "");
  AppendVarString(*buffer, &offset, "100");
  AppendVarString(*buffer, &offset, "futures.orders");
  return {buffer->data(), offset};
}

websocket::MessageView BinaryView(std::string_view payload) noexcept {
  return {
      .kind = websocket::PayloadKind::kBinary,
      .payload = std::as_bytes(std::span(payload.data(), payload.size())),
      .sequence = 1,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

struct CountingPublisher {
  std::uint64_t event_count{0};
  std::uint64_t gap_count{0};

  bool Publish(const OrderFeedbackEvent& event) noexcept {
    ++event_count;
    std::uint64_t local_order_id = event.local_order_id;
    benchmark::DoNotOptimize(local_order_id);
    return true;
  }

  bool PublishGlobalGap(OrderFeedbackGapReason, std::int64_t) noexcept {
    ++gap_count;
    return true;
  }
};

std::unique_ptr<OrderFeedbackShmChannel> MakeChannelForBenchmark() {
  auto channel = std::make_unique<OrderFeedbackShmChannel>();
  order_feedback_shm_detail::InitializeLaneHeaders(*channel);
  return channel;
}

websocket::ConnectionConfig MakeConfig() {
  websocket::ConnectionConfig config{};
  config.host = "localhost";
  config.service = "80";
  config.target = "/v4/ws/usdt";
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 4096;
  return config;
}

void BM_GateOrderFeedbackParserOneOrder(benchmark::State& state) {
  std::array<char, 512> buffer{};
  const std::string_view payload = BuildOrdersPayload(&buffer, {});
  OrderFeedbackParserStats stats{};
  std::uint64_t observed_local_order_id = 0;

  for (auto _ : state) {
    OrderFeedbackParseResult parsed = ParseGateOrderFeedbackMessage(
        payload, kLocalReceiveNs, stats,
        [&observed_local_order_id](const OrderFeedbackEvent& event) noexcept {
          observed_local_order_id = event.local_order_id;
        });
    benchmark::DoNotOptimize(parsed);
    benchmark::DoNotOptimize(observed_local_order_id);
    if (parsed.status != OrderFeedbackParseStatus::kOk ||
        parsed.events_emitted != 1 ||
        observed_local_order_id != kLocalOrderId) {
      state.SkipWithError("order feedback parse failed");
      return;
    }
  }

  benchmark::DoNotOptimize(stats);
  state.SetItemsProcessed(state.iterations());
}

void BM_GateOrderFeedbackSessionBinaryToCountingPublisher(
    benchmark::State& state) {
  std::array<char, 512> buffer{};
  const std::string_view payload = BuildOrdersPayload(&buffer, {});
  const websocket::MessageView view = BinaryView(payload);
  CountingPublisher publisher;
  using Session =
      OrderFeedbackSession<CountingPublisher,
                           OrderFeedbackSessionDefaultPlainWebSocketPolicy,
                           OrderFeedbackSessionDiagnostics>;
  Session session(MakeConfig(),
                  LoginCredentials{.api_key = "key", .api_secret = "secret"},
                  publisher);

  for (auto _ : state) {
    websocket::DeliveryResult result = session.Handle(view);
    benchmark::DoNotOptimize(result);
    if (result != websocket::DeliveryResult::kAccepted) {
      state.SkipWithError("order feedback session rejected binary payload");
      return;
    }
  }

  benchmark::DoNotOptimize(publisher.event_count);
  std::uint64_t events_published = session.stats().events_published;
  benchmark::DoNotOptimize(events_published);
  state.SetItemsProcessed(state.iterations());
}

void BM_GateOrderFeedbackSessionBinaryToShmPublisherThenDrain(
    benchmark::State& state) {
  std::array<char, 512> buffer{};
  const std::string_view payload = BuildOrdersPayload(&buffer, {});
  const websocket::MessageView view = BinaryView(payload);
  auto channel = MakeChannelForBenchmark();
  OrderFeedbackShmPublisher publisher(*channel);
  using Session =
      OrderFeedbackSession<OrderFeedbackShmPublisher,
                           OrderFeedbackSessionDefaultPlainWebSocketPolicy,
                           OrderFeedbackSessionDiagnostics>;
  Session session(MakeConfig(),
                  LoginCredentials{.api_key = "key", .api_secret = "secret"},
                  publisher);
  OrderFeedbackEvent popped{};

  for (auto _ : state) {
    websocket::DeliveryResult result = session.Handle(view);
    benchmark::DoNotOptimize(result);
    bool drained = channel->lanes[kStrategyId].queue.TryPop(popped);
    benchmark::DoNotOptimize(drained);
    benchmark::DoNotOptimize(popped);
    if (result != websocket::DeliveryResult::kAccepted || !drained) {
      state.SkipWithError("order feedback session SHM publish failed");
      return;
    }
  }

  benchmark::DoNotOptimize(publisher.published_count());
  std::uint64_t events_published = session.stats().events_published;
  benchmark::DoNotOptimize(events_published);
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_GateOrderFeedbackParserOneOrder)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_GateOrderFeedbackSessionBinaryToCountingPublisher)
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_GateOrderFeedbackSessionBinaryToShmPublisherThenDrain)
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila::gate
