#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/compile.h>
#include <fmt/format.h>

#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/trading/order_gateway_shm.h"
#include "core/websocket/active_spin_loop.h"
#include <sched.h>

namespace aquila::core {
namespace {

namespace benchmarking = websocket::benchmarking;

constexpr std::size_t kLatencyIterations = 4096;
constexpr std::uint32_t kQueueCapacity = 64;
constexpr std::chrono::seconds kSetupTimeout{2};

[[nodiscard]] bool BindCurrentThreadToCpu(int cpu_id) noexcept {
  if (cpu_id < 0 || cpu_id >= CPU_SETSIZE) {
    return false;
  }
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_id, &cpuset);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
}

[[nodiscard]] bool WaitUntilReady(const std::atomic<bool>& ready) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + kSetupTimeout;
  while (!ready.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    websocket::CpuRelax();
  }
  return true;
}

template <typename Predicate>
[[nodiscard]] bool SpinUntil(Predicate predicate) noexcept {
  constexpr std::size_t kClockCheckMask = (1U << 20U) - 1U;
  const auto deadline = std::chrono::steady_clock::now() + kSetupTimeout;
  std::size_t spins = 0;
  while (!predicate()) {
    if (((++spins) & kClockCheckMask) == 0 &&
        std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    websocket::CpuRelax();
  }
  return true;
}

class QueueFixture {
 public:
  explicit QueueFixture(std::uint16_t route_count) {
    shm_name_ =
        fmt::format(FMT_COMPILE("aquila_order_gateway_topology_{}_{}"),
                    static_cast<unsigned long long>(::getpid()),
                    static_cast<unsigned long long>(++next_instance_id_));
    auto manager_result = OrderGatewayShmManager::Create(OrderGatewayShmConfig{
        .shm_name = shm_name_,
        .create = true,
        .remove_existing = true,
        .route_count = route_count,
        .command_queue_capacity = kQueueCapacity,
        .event_queue_capacity = kQueueCapacity,
        .startup_ready_timeout_s = 1,
    });
    if (!manager_result.ok) {
      error_ = std::move(manager_result.error);
      return;
    }
    manager_ = std::move(manager_result.value);
    const std::string normalized_name = "/" + shm_name_;
    ::shm_unlink(normalized_name.c_str());
    ok_ = true;
  }

  [[nodiscard]] bool ok() const noexcept {
    return ok_;
  }

  [[nodiscard]] const std::string& error() const noexcept {
    return error_;
  }

  [[nodiscard]] OrderGatewayCommandQueue CommandQueue(
      std::uint16_t route) noexcept {
    return manager_.CommandQueue(route);
  }

  [[nodiscard]] OrderGatewayEventQueue EventQueue(
      std::uint16_t route) noexcept {
    return manager_.EventQueue(route);
  }

 private:
  static inline std::uint64_t next_instance_id_{0};

  std::string shm_name_;
  std::string error_;
  OrderGatewayShmManager manager_;
  bool ok_{false};
};

[[nodiscard]] OrderGatewayCommand MakeCommand(std::uint64_t sequence) noexcept {
  OrderGatewayCommand command{};
  command.command_seq = sequence;
  command.kind = OrderGatewayCommandKind::kPlace;
  command.payload.place = OrderPlaceRequest{
      .local_order_id = sequence,
      .parent_id = sequence,
      .price = 80000.0,
      .quantity = 1.0,
      .symbol_id = 1,
      .gateway_route_id = 0,
      .exchange = Exchange::kGate,
      .side = OrderSide::kBuy,
      .order_type = OrderType::kLimit,
      .time_in_force = TimeInForce::kGoodTillCancel,
      .price_decimal_places = 0,
      .quantity_decimal_places = 0,
  };
  SetOrderSymbol(&command.payload.place, "BTC_USDT");
  return command;
}

[[nodiscard]] OrderGatewayEvent MakeEvent(std::uint64_t sequence,
                                          std::uint16_t route_id = 0) noexcept {
  return {
      .event_seq = sequence,
      .command_seq = sequence,
      .parent_id = sequence,
      .local_order_id = sequence,
      .route_id = route_id,
      .kind = OrderGatewayEventKind::kOrderResponse,
      .command_kind = OrderGatewayCommandKind::kPlace,
      .response_kind = OrderResponseKind::kAccepted,
  };
}

void SetThreadPairLabel(benchmark::State& state, int producer_cpu,
                        int consumer_cpu) {
  state.SetLabel(fmt::format(FMT_COMPILE("producer_cpu={} consumer_cpu={}"),
                             producer_cpu, consumer_cpu));
}

void BM_CommandOneWayHandoff(benchmark::State& state) {
  const int producer_cpu = static_cast<int>(state.range(0));
  const int consumer_cpu = static_cast<int>(state.range(1));
  if (!BindCurrentThreadToCpu(producer_cpu)) {
    state.SkipWithError("failed to bind producer benchmark thread");
    return;
  }

  QueueFixture fixture{1};
  if (!fixture.ok()) {
    state.SkipWithError(fixture.error().c_str());
    return;
  }
  OrderGatewayCommandQueue producer_queue = fixture.CommandQueue(0);
  OrderGatewayCommandQueue consumer_queue = fixture.CommandQueue(0);
  std::vector<std::uint64_t> samples_ns(kLatencyIterations);
  std::atomic<bool> consumer_ready{false};
  std::atomic<bool> consumer_bind_ok{false};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> consumed_sequence{0};

  std::thread consumer([&] {
    consumer_bind_ok.store(BindCurrentThreadToCpu(consumer_cpu),
                           std::memory_order_relaxed);
    consumer_ready.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < kLatencyIterations; ++i) {
      OrderGatewayCommand command{};
      while (!consumer_queue.TryPop(&command)) {
        if (stop.load(std::memory_order_relaxed)) {
          return;
        }
        websocket::CpuRelax();
      }
      const std::uint64_t received_ns = benchmarking::NowNs();
      samples_ns[i] =
          received_ns - static_cast<std::uint64_t>(command.owner_enqueue_ns);
      consumed_sequence.store(command.command_seq, std::memory_order_release);
    }
  });

  if (!WaitUntilReady(consumer_ready) ||
      !consumer_bind_ok.load(std::memory_order_relaxed)) {
    stop.store(true, std::memory_order_relaxed);
    consumer.join();
    state.SkipWithError("failed to start bound consumer thread");
    return;
  }

  std::size_t index = 0;
  for (auto _ : state) {
    (void)_;
    const std::uint64_t sequence = index + 1U;
    OrderGatewayCommand command = MakeCommand(sequence);
    command.owner_enqueue_ns = static_cast<std::int64_t>(benchmarking::NowNs());
    if (!producer_queue.TryPush(command)) {
      state.SkipWithError("command queue unexpectedly full");
      break;
    }
    if (!SpinUntil([&] {
          return consumed_sequence.load(std::memory_order_acquire) >= sequence;
        })) {
      state.SkipWithError("consumer handoff timed out");
      break;
    }
    state.SetIterationTime(static_cast<double>(samples_ns[index]) /
                           1'000'000'000.0);
    ++index;
  }

  stop.store(true, std::memory_order_relaxed);
  consumer.join();
  samples_ns.resize(index);
  benchmarking::SetLatencyCounters(state, std::move(samples_ns), "commands",
                                   index);
  SetThreadPairLabel(state, producer_cpu, consumer_cpu);
}

void BM_CommandEventRoundTrip(benchmark::State& state) {
  const int owner_cpu = static_cast<int>(state.range(0));
  const int worker_cpu = static_cast<int>(state.range(1));
  if (!BindCurrentThreadToCpu(owner_cpu)) {
    state.SkipWithError("failed to bind owner benchmark thread");
    return;
  }

  QueueFixture fixture{1};
  if (!fixture.ok()) {
    state.SkipWithError(fixture.error().c_str());
    return;
  }
  OrderGatewayCommandQueue producer_command_queue = fixture.CommandQueue(0);
  OrderGatewayCommandQueue worker_command_queue = fixture.CommandQueue(0);
  OrderGatewayEventQueue worker_event_queue = fixture.EventQueue(0);
  OrderGatewayEventQueue consumer_event_queue = fixture.EventQueue(0);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);
  std::atomic<bool> worker_ready{false};
  std::atomic<bool> worker_bind_ok{false};
  std::atomic<bool> worker_failed{false};
  std::atomic<bool> stop{false};

  std::thread worker([&] {
    worker_bind_ok.store(BindCurrentThreadToCpu(worker_cpu),
                         std::memory_order_relaxed);
    worker_ready.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < kLatencyIterations; ++i) {
      OrderGatewayCommand command{};
      while (!worker_command_queue.TryPop(&command)) {
        if (stop.load(std::memory_order_relaxed)) {
          return;
        }
        websocket::CpuRelax();
      }
      OrderGatewayEvent event{};
      event.event_seq = command.command_seq;
      event.command_seq = command.command_seq;
      event.parent_id = OrderGatewayCommandParentId(command);
      event.local_order_id = OrderGatewayCommandLocalOrderId(command);
      event.worker_dequeue_ns =
          static_cast<std::int64_t>(benchmarking::NowNs());
      event.worker_event_enqueue_ns = event.worker_dequeue_ns;
      event.route_id = OrderGatewayCommandRouteId(command);
      event.kind = OrderGatewayEventKind::kOrderResponse;
      event.command_kind = command.kind;
      event.response_kind = OrderResponseKind::kAccepted;
      if (!worker_event_queue.TryPush(event)) {
        worker_failed.store(true, std::memory_order_release);
        return;
      }
    }
  });

  if (!WaitUntilReady(worker_ready) ||
      !worker_bind_ok.load(std::memory_order_relaxed)) {
    stop.store(true, std::memory_order_relaxed);
    worker.join();
    state.SkipWithError("failed to start bound worker thread");
    return;
  }

  std::uint64_t sequence = 0;
  for (auto _ : state) {
    (void)_;
    ++sequence;
    OrderGatewayCommand command = MakeCommand(sequence);
    const std::uint64_t start_ns = benchmarking::NowNs();
    command.owner_enqueue_ns = static_cast<std::int64_t>(start_ns);
    if (!producer_command_queue.TryPush(command)) {
      state.SkipWithError("command queue unexpectedly full");
      break;
    }
    OrderGatewayEvent event{};
    bool worker_stopped = false;
    const bool received = SpinUntil([&] {
      if (consumer_event_queue.TryPop(&event)) {
        return true;
      }
      if (worker_failed.load(std::memory_order_acquire)) {
        worker_stopped = true;
        return true;
      }
      return false;
    });
    if (!received) {
      state.SkipWithError("command/event round trip timed out");
      break;
    }
    if (worker_stopped) {
      state.SkipWithError("worker event queue unexpectedly full");
      break;
    }
    if (event.command_seq != sequence) {
      state.SkipWithError("round-trip event sequence mismatch");
      break;
    }
    const std::uint64_t elapsed_ns = benchmarking::NowNs() - start_ns;
    samples_ns.push_back(elapsed_ns);
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
  }

  stop.store(true, std::memory_order_relaxed);
  worker.join();
  benchmarking::SetLatencyCounters(state, std::move(samples_ns), "round_trips",
                                   sequence);
  SetThreadPairLabel(state, owner_cpu, worker_cpu);
}

void BM_CommandEnqueueLowerBound(benchmark::State& state) {
  const int producer_cpu = static_cast<int>(state.range(0));
  if (!BindCurrentThreadToCpu(producer_cpu)) {
    state.SkipWithError("failed to bind enqueue benchmark thread");
    return;
  }
  QueueFixture fixture{1};
  if (!fixture.ok()) {
    state.SkipWithError(fixture.error().c_str());
    return;
  }
  OrderGatewayCommandQueue producer_queue = fixture.CommandQueue(0);
  OrderGatewayCommandQueue cleanup_queue = fixture.CommandQueue(0);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);
  std::uint64_t sequence = 0;
  for (auto _ : state) {
    (void)_;
    OrderGatewayCommand command = MakeCommand(++sequence);
    const std::uint64_t start_ns = benchmarking::NowNs();
    if (!producer_queue.TryPush(command)) {
      state.SkipWithError("enqueue lower-bound queue unexpectedly full");
      return;
    }
    const std::uint64_t elapsed_ns = benchmarking::NowNs() - start_ns;
    OrderGatewayCommand consumed{};
    if (!cleanup_queue.TryPop(&consumed) ||
        consumed.command_seq != command.command_seq) {
      state.SkipWithError("enqueue lower-bound cleanup mismatch");
      return;
    }
    samples_ns.push_back(elapsed_ns);
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
  }
  benchmarking::SetLatencyCounters(state, std::move(samples_ns), "commands",
                                   sequence);
  state.SetLabel(fmt::format(FMT_COMPILE("producer_cpu={}"), producer_cpu));
}

void BM_CommandBurstDrain(benchmark::State& state) {
  const int consumer_cpu = static_cast<int>(state.range(0));
  const std::size_t burst_size = static_cast<std::size_t>(state.range(1));
  if (!BindCurrentThreadToCpu(consumer_cpu)) {
    state.SkipWithError("failed to bind command drain benchmark thread");
    return;
  }
  QueueFixture fixture{1};
  if (!fixture.ok()) {
    state.SkipWithError(fixture.error().c_str());
    return;
  }
  OrderGatewayCommandQueue producer_queue = fixture.CommandQueue(0);
  OrderGatewayCommandQueue consumer_queue = fixture.CommandQueue(0);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);
  std::uint64_t sequence = 0;
  for (auto _ : state) {
    (void)_;
    const std::uint64_t first_sequence = sequence + 1;
    for (std::size_t i = 0; i < burst_size; ++i) {
      if (!producer_queue.TryPush(MakeCommand(++sequence))) {
        state.SkipWithError("command burst prefill unexpectedly full");
        return;
      }
    }
    const std::uint64_t start_ns = benchmarking::NowNs();
    for (std::size_t i = 0; i < burst_size; ++i) {
      OrderGatewayCommand command{};
      if (!consumer_queue.TryPop(&command) ||
          command.command_seq != first_sequence + i) {
        state.SkipWithError("command burst drain sequence mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = benchmarking::NowNs() - start_ns;
    samples_ns.push_back(elapsed_ns);
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
  }
  benchmarking::SetLatencyCounters(state, std::move(samples_ns), "commands",
                                   sequence);
  state.SetLabel(fmt::format(FMT_COMPILE("consumer_cpu={} burst={}"),
                             consumer_cpu, burst_size));
}

void BM_EventBurstDrain(benchmark::State& state) {
  const int consumer_cpu = static_cast<int>(state.range(0));
  const std::size_t burst_size = static_cast<std::size_t>(state.range(1));
  if (!BindCurrentThreadToCpu(consumer_cpu)) {
    state.SkipWithError("failed to bind event drain benchmark thread");
    return;
  }
  QueueFixture fixture{1};
  if (!fixture.ok()) {
    state.SkipWithError(fixture.error().c_str());
    return;
  }
  OrderGatewayEventQueue producer_queue = fixture.EventQueue(0);
  OrderGatewayEventQueue consumer_queue = fixture.EventQueue(0);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);
  std::uint64_t sequence = 0;
  for (auto _ : state) {
    (void)_;
    const std::uint64_t first_sequence = sequence + 1;
    for (std::size_t i = 0; i < burst_size; ++i) {
      if (!producer_queue.TryPush(MakeEvent(++sequence))) {
        state.SkipWithError("event burst prefill unexpectedly full");
        return;
      }
    }
    const std::uint64_t start_ns = benchmarking::NowNs();
    for (std::size_t i = 0; i < burst_size; ++i) {
      OrderGatewayEvent event{};
      if (!consumer_queue.TryPop(&event) ||
          event.command_seq != first_sequence + i) {
        state.SkipWithError("event burst drain sequence mismatch");
        return;
      }
    }
    const std::uint64_t elapsed_ns = benchmarking::NowNs() - start_ns;
    samples_ns.push_back(elapsed_ns);
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
  }
  benchmarking::SetLatencyCounters(state, std::move(samples_ns), "events",
                                   sequence);
  state.SetLabel(fmt::format(FMT_COMPILE("consumer_cpu={} burst={}"),
                             consumer_cpu, burst_size));
}

void BM_EventEmptyPoll(benchmark::State& state) {
  const int consumer_cpu = static_cast<int>(state.range(0));
  const std::uint16_t route_count = static_cast<std::uint16_t>(state.range(1));
  if (!BindCurrentThreadToCpu(consumer_cpu)) {
    state.SkipWithError("failed to bind empty-poll benchmark thread");
    return;
  }

  QueueFixture fixture{route_count};
  if (!fixture.ok()) {
    state.SkipWithError(fixture.error().c_str());
    return;
  }
  std::array<OrderGatewayEventQueue, 4> queues{};
  for (std::uint16_t route = 0; route < route_count; ++route) {
    queues[route] = fixture.EventQueue(route);
  }

  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);
  for (auto _ : state) {
    (void)_;
    const std::uint64_t start_ns = benchmarking::NowNs();
    std::uint64_t popped = 0;
    for (std::uint16_t route = 0; route < route_count; ++route) {
      OrderGatewayEvent event{};
      popped += queues[route].TryPop(&event) ? 1U : 0U;
    }
    const std::uint64_t elapsed_ns = benchmarking::NowNs() - start_ns;
    if (popped != 0) {
      state.SkipWithError("empty event queue unexpectedly contained data");
      return;
    }
    samples_ns.push_back(elapsed_ns);
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
  }

  benchmarking::SetLatencyCounters(state, std::move(samples_ns), "routes",
                                   route_count);
  state.SetLabel(fmt::format(FMT_COMPILE("consumer_cpu={} routes={}"),
                             consumer_cpu, route_count));
}

void BM_EventRouteScanOneReady(benchmark::State& state) {
  const int consumer_cpu = static_cast<int>(state.range(0));
  const std::uint16_t route_count = static_cast<std::uint16_t>(state.range(1));
  if (!BindCurrentThreadToCpu(consumer_cpu)) {
    state.SkipWithError("failed to bind event scan benchmark thread");
    return;
  }
  QueueFixture fixture{route_count};
  if (!fixture.ok()) {
    state.SkipWithError(fixture.error().c_str());
    return;
  }
  std::array<OrderGatewayEventQueue, 4> queues{};
  for (std::uint16_t route = 0; route < route_count; ++route) {
    queues[route] = fixture.EventQueue(route);
  }
  OrderGatewayEventQueue ready_queue = fixture.EventQueue(route_count - 1);
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kLatencyIterations);
  std::uint64_t sequence = 0;
  for (auto _ : state) {
    (void)_;
    const OrderGatewayEvent expected = MakeEvent(++sequence, route_count - 1);
    if (!ready_queue.TryPush(expected)) {
      state.SkipWithError("event scan prefill unexpectedly full");
      return;
    }
    const std::uint64_t start_ns = benchmarking::NowNs();
    std::uint64_t popped = 0;
    OrderGatewayEvent received{};
    for (std::uint16_t route = 0; route < route_count; ++route) {
      OrderGatewayEvent event{};
      if (queues[route].TryPop(&event)) {
        ++popped;
        received = event;
      }
    }
    const std::uint64_t elapsed_ns = benchmarking::NowNs() - start_ns;
    if (popped != 1 || received.command_seq != expected.command_seq ||
        received.route_id != expected.route_id) {
      state.SkipWithError("event route scan result mismatch");
      return;
    }
    samples_ns.push_back(elapsed_ns);
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
  }
  benchmarking::SetLatencyCounters(state, std::move(samples_ns), "events",
                                   sequence);
  state.SetLabel(fmt::format(FMT_COMPILE("consumer_cpu={} routes={}"),
                             consumer_cpu, route_count));
}

BENCHMARK(BM_CommandEnqueueLowerBound)
    ->Arg(29)
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_CommandOneWayHandoff)
    ->Args({29, 16})
    ->Args({29, 17})
    ->Args({29, 18})
    ->Args({29, 19})
    ->Args({29, 30})
    ->Args({29, 31})
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_CommandEventRoundTrip)
    ->Args({29, 16})
    ->Args({29, 17})
    ->Args({29, 18})
    ->Args({29, 19})
    ->Args({29, 30})
    ->Args({29, 31})
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_EventEmptyPoll)
    ->Args({29, 1})
    ->Args({29, 4})
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_EventRouteScanOneReady)
    ->Args({29, 1})
    ->Args({29, 4})
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_CommandBurstDrain)
    ->Args({29, 1})
    ->Args({29, 4})
    ->Args({29, 16})
    ->Args({29, 64})
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_EventBurstDrain)
    ->Args({29, 1})
    ->Args({29, 4})
    ->Args({29, 16})
    ->Args({29, 64})
    ->Iterations(kLatencyIterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aquila::core
