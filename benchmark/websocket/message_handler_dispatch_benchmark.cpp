#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <benchmark/benchmark.h>

#include "core/websocket/message_view.h"

namespace ws = aquila::websocket;

namespace {

#if defined(__GNUC__) || defined(__clang__)
#define AQUILA_NOINLINE __attribute__((noinline))
#else
#define AQUILA_NOINLINE
#endif

struct DispatchState {
  std::uint64_t calls{0};
  std::uint64_t bytes{0};
};

AQUILA_NOINLINE ws::DeliveryResult RecordCallback(
    void* context, const ws::MessageView& view) noexcept {
  auto* state = static_cast<DispatchState*>(context);
  ++state->calls;
  state->bytes += view.payload.size();
  return ws::DeliveryResult::kAccepted;
}

struct TypedHandler {
  std::uint64_t calls{0};
  std::uint64_t bytes{0};

  ws::DeliveryResult Handle(const ws::MessageView& view) noexcept {
    ++calls;
    bytes += view.payload.size();
    return ws::DeliveryResult::kAccepted;
  }
};

AQUILA_NOINLINE ws::MessageCallback MakeOpaqueCallback(
    DispatchState* state) noexcept {
  return {.context = state, .handler = &RecordCallback};
}

AQUILA_NOINLINE ws::MessageHandlerRef<TypedHandler> MakeOpaqueTypedHandler(
    TypedHandler& handler) noexcept {
  return ws::MakeMessageHandler(handler);
}

ws::MessageView BuildMessageView(std::span<const std::byte> payload) noexcept {
  return {
      .kind = ws::PayloadKind::kBinary,
      .payload = payload,
      .sequence = 1,
      .fin = true,
      .readable_tail_bytes = 0,
  };
}

void BenchmarkMessageCallbackDispatch(benchmark::State& state) {
  const std::array<std::byte, 32> payload{};
  const ws::MessageView view = BuildMessageView(payload);
  DispatchState dispatch_state{};
  const ws::MessageCallback callback = MakeOpaqueCallback(&dispatch_state);

  for (auto _ : state) {
    ws::DeliveryResult result = callback.Handle(view);
    benchmark::DoNotOptimize(result);
  }

  benchmark::DoNotOptimize(dispatch_state);
  state.SetItemsProcessed(static_cast<std::int64_t>(dispatch_state.calls));
  state.counters["bytes"] = static_cast<double>(dispatch_state.bytes);
}

BENCHMARK(BenchmarkMessageCallbackDispatch)
    ->Name("message_handler_dispatch/message_callback")
    ->Unit(benchmark::kNanosecond);

void BenchmarkTypedHandlerDispatch(benchmark::State& state) {
  const std::array<std::byte, 32> payload{};
  const ws::MessageView view = BuildMessageView(payload);
  TypedHandler typed_handler{};
  const auto handler = MakeOpaqueTypedHandler(typed_handler);

  for (auto _ : state) {
    ws::DeliveryResult result = handler.Handle(view);
    benchmark::DoNotOptimize(result);
  }

  benchmark::DoNotOptimize(typed_handler);
  state.SetItemsProcessed(static_cast<std::int64_t>(typed_handler.calls));
  state.counters["bytes"] = static_cast<double>(typed_handler.bytes);
}

BENCHMARK(BenchmarkTypedHandlerDispatch)
    ->Name("message_handler_dispatch/typed_handler_ref")
    ->Unit(benchmark::kNanosecond);

void BenchmarkTypedHandlerValueDispatch(benchmark::State& state) {
  const std::array<std::byte, 32> payload{};
  const ws::MessageView view = BuildMessageView(payload);
  TypedHandler typed_handler{};

  for (auto _ : state) {
    ws::DeliveryResult result = typed_handler.Handle(view);
    benchmark::DoNotOptimize(result);
  }

  benchmark::DoNotOptimize(typed_handler);
  state.SetItemsProcessed(static_cast<std::int64_t>(typed_handler.calls));
  state.counters["bytes"] = static_cast<double>(typed_handler.bytes);
}

BENCHMARK(BenchmarkTypedHandlerValueDispatch)
    ->Name("message_handler_dispatch/typed_handler_value")
    ->Unit(benchmark::kNanosecond);

}  // namespace
