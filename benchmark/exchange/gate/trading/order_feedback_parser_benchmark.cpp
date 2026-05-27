#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <benchmark/benchmark.h>

#include "core/trading/order_feedback_shm.h"
#include "core/websocket/message_view.h"
#include "evaluation/exchange/gate/trading/order_feedback_payload_builder.h"
#include "exchange/gate/trading/order_feedback_parser.h"
#include "exchange/gate/trading/order_feedback_session.h"

namespace aquila::gate {
namespace {

using evaluation::BuildOrderFeedbackOrdersPayload;

constexpr std::uint8_t kStrategyId =
    evaluation::kOrderFeedbackPayloadStrategyId;
constexpr std::uint64_t kLocalOrderId =
    evaluation::kOrderFeedbackPayloadLocalOrderId;
constexpr std::int64_t kLocalReceiveNs = 1'770'000'000'001'222'000;

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
  std::uint64_t continuity_lost_count{0};

  bool Publish(const OrderFeedbackEvent& event) noexcept {
    ++event_count;
    std::uint64_t local_order_id = event.local_order_id;
    benchmark::DoNotOptimize(local_order_id);
    return true;
  }

  bool PublishGlobalContinuityLost(OrderFeedbackContinuityReason,
                                   std::int64_t) noexcept {
    ++continuity_lost_count;
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
  config.port = "80";
  config.target = "/v4/ws/usdt";
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 4096;
  return config;
}

void BM_GateOrderFeedbackParserOneOrder(benchmark::State& state) {
  std::array<char, 512> buffer{};
  const std::string_view payload = BuildOrderFeedbackOrdersPayload(&buffer);
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
  const std::string_view payload = BuildOrderFeedbackOrdersPayload(&buffer);
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
  const std::string_view payload = BuildOrderFeedbackOrdersPayload(&buffer);
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
