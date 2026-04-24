#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/active_spin_loop.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/types.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

struct RuntimeContext {
  std::atomic<size_t> delivered{0};
  std::atomic<std::uint64_t> send_ns{0};
  std::atomic<bool> timing_anomaly{false};
  std::vector<std::uint64_t>* samples_ns{nullptr};
};

DeliveryResult RecordLoopback(void* context, const MessageView&) noexcept {
  auto* runtime = static_cast<RuntimeContext*>(context);
  const size_t index = runtime->delivered.load();
  if (runtime->samples_ns == nullptr || index >= runtime->samples_ns->size()) {
    return DeliveryResult::kFatal;
  }
  const std::uint64_t receive_ns = NowNs();
  const std::uint64_t send_ns = runtime->send_ns.load();
  if (receive_ns < send_ns) {
    runtime->timing_anomaly.store(true);
    return DeliveryResult::kFatal;
  }
  (*runtime->samples_ns)[index] = receive_ns - send_ns;
  runtime->delivered.store(index + 1);
  return DeliveryResult::kAccepted;
}

template <typename SessionT>
struct RuntimeSession {
  SessionT& session;
  std::atomic<bool>& stop_flag;

  void DriveWrite() noexcept { session.DriveWrite(); }

  void DriveRead() noexcept { session.DriveRead(); }

  void AdvanceHeartbeat(std::uint64_t now_ns) noexcept {
    session.AdvanceHeartbeat(now_ns);
  }

  bool ShouldReconnect() const noexcept {
    return stop_flag.load() || session.ShouldReconnect();
  }
};

void BenchmarkRuntimeLoopback(benchmark::State& state) {
  constexpr size_t kSamples = 2048;
  ConnectionConfig config{};
  config.enable_tls = false;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 128;
  config.read_buffer_bytes = 4096;
  config.frame_buffer_bytes = 4096;
  config.heartbeat_interval_ms = std::numeric_limits<std::uint32_t>::max();
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();
  config.runtime_policy.affinity_mode = AffinityMode::kNone;
  config.runtime_policy.lock_memory = false;
  config.runtime_policy.prefault_stack = false;

  SocketPair pair;
  if (!CreateSocketPair(pair)) {
    state.SkipWithError("socketpair create failed");
    return;
  }

  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  std::vector<std::uint64_t> samples_ns(kSamples, 0);
  RuntimeContext runtime_context{};
  runtime_context.samples_ns = &samples_ns;
  MessageConsumer consumer{&runtime_context, &RecordLoopback};
  CriticalSession<LocalFdSocket> session(config, pair.client, arena, metrics);
  session.SetConsumer(consumer);

  std::atomic<bool> stop_flag{false};
  std::atomic<bool> owner_finished{false};
  RuntimeSession runtime_session{session, stop_flag};
  ActiveSpinLoop loop(config.runtime_policy);
  std::thread owner_thread([&]() {
    loop.Run(runtime_session);
    owner_finished.store(true);
  });

  const auto frame = BuildServerTextFrame("tick");
  size_t sample_index = 0;
  for (auto _ : state) {
    runtime_context.send_ns.store(NowNs());
    if (!WriteAllFd(pair.peer_fd, frame)) {
      stop_flag.store(true);
      owner_thread.join();
      state.SkipWithError("peer write failed");
      return;
    }
    while (runtime_context.delivered.load() <= sample_index) {
      if (runtime_context.timing_anomaly.load()) {
        stop_flag.store(true);
        owner_thread.join();
        state.SkipWithError("timing anomaly");
        return;
      }
      if (owner_finished.load()) {
        stop_flag.store(true);
        owner_thread.join();
        state.SkipWithError("session requested reconnect");
        return;
      }
      std::this_thread::yield();
    }
    const std::uint64_t elapsed_ns = samples_ns[sample_index];
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    ++sample_index;
  }

  stop_flag.store(true);
  owner_thread.join();

  SetLatencyCounters(state, std::move(samples_ns), "messages",
                     metrics.rx_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "local-socketpair",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkRuntimeLoopback)
    ->Name("runtime_loopback")
    ->Iterations(2048)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
