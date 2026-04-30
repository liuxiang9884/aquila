#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/active_spin_loop.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/runtime_policy.h"
#include "core/websocket/types.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#if defined(__linux__)
#include <sched.h>
#endif

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

constexpr size_t kSamples = 2048;

struct RuntimeContext {
  std::atomic<size_t> delivered{0};
  std::atomic<std::uint64_t> send_ns{0};
  std::atomic<bool> timing_anomaly{false};
  std::vector<std::uint64_t>* samples_ns{nullptr};
};

struct VariantSpec {
  std::string_view name;
  RuntimePolicy runtime_policy;
};

struct VariantContext {
  std::atomic<bool> owner_finished{false};
  std::atomic<int> apply_status{0};
  RuntimeContext runtime_context{};
  std::vector<std::uint64_t> samples_ns{};
  std::string owner_affinity{};
  std::string owner_scheduling{};
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

ConnectionConfig MakeConfig(const RuntimePolicy& runtime_policy) {
  ConnectionConfig config{};
  config.enable_tls = false;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 128;
  config.read_buffer_bytes = 4096;
  config.frame_buffer_bytes = 4096;
  config.heartbeat_interval_ms = std::numeric_limits<std::uint32_t>::max();
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();
  config.runtime_policy = runtime_policy;
  return config;
}

bool FirstAvailableCpu(int* cpu_id) noexcept {
  if (cpu_id == nullptr) {
    return false;
  }

#if defined(__linux__)
  cpu_set_t affinity_mask;
  CPU_ZERO(&affinity_mask);
  if (sched_getaffinity(0, sizeof(affinity_mask), &affinity_mask) != 0) {
    return false;
  }
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &affinity_mask)) {
      *cpu_id = cpu;
      return true;
    }
  }
#endif

  return false;
}

void RunAffinityVariant(benchmark::State& state, const VariantSpec& variant) {
  if (variant.runtime_policy.affinity_mode != AffinityMode::kNone &&
      variant.runtime_policy.io_cpu_id < 0) {
    state.SkipWithError("no available CPU");
    return;
  }

  VariantContext variant_context;
  variant_context.samples_ns.assign(kSamples, 0);
  variant_context.runtime_context.samples_ns = &variant_context.samples_ns;
  SocketPair pair;
  if (!CreateSocketPair(pair)) {
    state.SkipWithError("socketpair create failed");
    return;
  }

  const ConnectionConfig config = MakeConfig(variant.runtime_policy);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  MessageCallback consumer{&variant_context.runtime_context, &RecordLoopback};
  CriticalSession<LocalFdSocket> session(config, pair.client, arena, metrics);
  session.SetMessageCallback(consumer);

  std::atomic<bool> stop_flag{false};
  RuntimeSession runtime_session{session, stop_flag};
  ActiveSpinLoop loop(config.runtime_policy);

  std::thread owner_thread([&]() {
    variant_context.apply_status.store(
        ApplyRuntimePolicy(config.runtime_policy) ? 1 : -1);
    if (variant_context.apply_status.load() < 0) {
      stop_flag.store(true);
      variant_context.owner_finished.store(true);
      return;
    }
    variant_context.owner_affinity = FormatAffinity();
    variant_context.owner_scheduling = FormatSchedulingPolicy();
    loop.Run(runtime_session);
    variant_context.owner_finished.store(true);
  });

  while (variant_context.apply_status.load() == 0) {
    std::this_thread::yield();
  }

  if (variant_context.apply_status.load() < 0) {
    stop_flag.store(true);
    owner_thread.join();
    state.SkipWithError("apply runtime policy failed");
    return;
  }

  const auto frame = BuildServerTextFrame("tick");
  size_t sample_index = 0;
  for (auto _ : state) {
    variant_context.runtime_context.send_ns.store(NowNs());
    if (!WriteAllFd(pair.peer_fd, frame)) {
      stop_flag.store(true);
      owner_thread.join();
      state.SkipWithError("peer write failed");
      return;
    }
    while (variant_context.runtime_context.delivered.load() <= sample_index) {
      if (variant_context.runtime_context.timing_anomaly.load()) {
        stop_flag.store(true);
        owner_thread.join();
        state.SkipWithError("timing anomaly");
        return;
      }
      if (variant_context.owner_finished.load()) {
        stop_flag.store(true);
        owner_thread.join();
        state.SkipWithError("session requested reconnect");
        return;
      }
      std::this_thread::yield();
    }
    const std::uint64_t elapsed_ns = variant_context.samples_ns[sample_index];
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    ++sample_index;
  }

  stop_flag.store(true);
  owner_thread.join();
  if (session.ShouldReconnect()) {
    state.SkipWithError("session requested reconnect");
    return;
  }

  const std::string_view owner_affinity =
      variant_context.owner_affinity.empty()
          ? std::string_view("unknown")
          : std::string_view(variant_context.owner_affinity);
  const std::string_view owner_scheduling =
      variant_context.owner_scheduling.empty()
          ? std::string_view("unknown")
          : std::string_view(variant_context.owner_scheduling);
  SetLatencyCounters(state, std::move(variant_context.samples_ns), "messages",
                     metrics.rx_messages);
  state.counters["affinity_mode"] =
      static_cast<double>(variant.runtime_policy.affinity_mode);
  state.counters["io_cpu_id"] =
      static_cast<double>(variant.runtime_policy.io_cpu_id);
  state.counters["lock_memory"] =
      variant.runtime_policy.lock_memory ? 1.0 : 0.0;
  state.counters["prefault_stack"] =
      variant.runtime_policy.prefault_stack ? 1.0 : 0.0;
  state.SetLabel(BuildBenchmarkLabel(false, "local-socketpair", owner_affinity,
                                     owner_scheduling));
}

VariantSpec MakeBaselineVariant() {
  RuntimePolicy policy{};
  policy.affinity_mode = AffinityMode::kNone;
  policy.io_cpu_id = -1;
  policy.lock_memory = false;
  policy.prefault_stack = false;
  return {"baseline", policy};
}

VariantSpec MakePinnedVariant(std::string_view name, bool prefault_stack,
                              bool lock_memory) {
  int first_cpu = -1;
  const bool has_cpu = FirstAvailableCpu(&first_cpu);
  RuntimePolicy policy{};
  policy.affinity_mode = AffinityMode::kRequired;
  policy.io_cpu_id = has_cpu ? first_cpu : -1;
  policy.lock_memory = lock_memory;
  policy.prefault_stack = prefault_stack;
  return {name, policy};
}

void BenchmarkAffinityBaseline(benchmark::State& state) {
  RunAffinityVariant(state, MakeBaselineVariant());
}

void BenchmarkAffinityPinned(benchmark::State& state) {
  RunAffinityVariant(state, MakePinnedVariant("pinned", false, false));
}

void BenchmarkAffinityPinnedPrefault(benchmark::State& state) {
  RunAffinityVariant(state, MakePinnedVariant("pinned_prefault", true, false));
}

void BenchmarkAffinityPinnedLocked(benchmark::State& state) {
  RunAffinityVariant(state, MakePinnedVariant("pinned_locked", true, true));
}

BENCHMARK(BenchmarkAffinityBaseline)
    ->Name("affinity_policy_comparison/baseline")
    ->Iterations(kSamples)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BenchmarkAffinityPinned)
    ->Name("affinity_policy_comparison/pinned")
    ->Iterations(kSamples)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BenchmarkAffinityPinnedPrefault)
    ->Name("affinity_policy_comparison/pinned_prefault")
    ->Iterations(kSamples)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BenchmarkAffinityPinnedLocked)
    ->Name("affinity_policy_comparison/pinned_locked")
    ->Iterations(kSamples)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
