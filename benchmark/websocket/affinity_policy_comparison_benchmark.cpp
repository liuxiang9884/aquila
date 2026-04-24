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
#include <cstdio>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
  std::vector<std::uint64_t>* samples_ns{nullptr};
};

struct VariantSpec {
  std::string_view name;
  RuntimePolicy runtime_policy;
};

struct VariantResult {
  std::vector<std::uint64_t> samples_ns;
  std::uint64_t rx_messages{0};
  bool success{false};
  const char* failure_reason{nullptr};
};

DeliveryResult RecordLoopback(void* context, const MessageView&) noexcept {
  auto* runtime = static_cast<RuntimeContext*>(context);
  const size_t index = runtime->delivered.fetch_add(1);
  if (runtime->samples_ns == nullptr || index >= runtime->samples_ns->size()) {
    return DeliveryResult::kFatal;
  }
  const std::uint64_t receive_ns = NowNs();
  const std::uint64_t send_ns = runtime->send_ns.load();
  (*runtime->samples_ns)[index] =
      receive_ns >= send_ns ? (receive_ns - send_ns) : 0;
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

VariantResult RunVariant(const VariantSpec& variant) {
  VariantResult result;
  result.samples_ns.assign(kSamples, 0);

  SocketPair pair;
  if (!CreateSocketPair(pair)) {
    result.failure_reason = "socketpair_create_failed";
    result.samples_ns.clear();
    return result;
  }

  const ConnectionConfig config = MakeConfig(variant.runtime_policy);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  RuntimeContext runtime_context{};
  runtime_context.samples_ns = &result.samples_ns;
  MessageConsumer consumer{&runtime_context, &RecordLoopback};
  CriticalSession<LocalFdSocket> session(config, pair.client, arena, metrics);
  session.SetConsumer(consumer);

  std::atomic<bool> stop_flag{false};
  std::atomic<int> apply_status{0};
  RuntimeSession runtime_session{session, stop_flag};
  ActiveSpinLoop loop(config.runtime_policy);

  std::thread owner_thread([&]() {
    apply_status.store(ApplyRuntimePolicy(config.runtime_policy) ? 1 : -1);
    if (apply_status.load() < 0) {
      stop_flag.store(true);
      return;
    }
    loop.Run(runtime_session);
  });

  while (apply_status.load() == 0) {
    std::this_thread::yield();
  }

  if (apply_status.load() < 0) {
    stop_flag.store(true);
    owner_thread.join();
    result.failure_reason = "apply_runtime_policy_failed";
    result.samples_ns.clear();
    return result;
  }

  const auto frame = BuildServerTextFrame("tick");
  for (size_t sample_index = 0; sample_index < kSamples; ++sample_index) {
    runtime_context.send_ns.store(NowNs());
    if (!WriteAllFd(pair.peer_fd, frame)) {
      stop_flag.store(true);
      owner_thread.join();
      result.failure_reason = "peer_write_failed";
      result.samples_ns.clear();
      return result;
    }
    while (runtime_context.delivered.load() <= sample_index) {
      if (session.ShouldReconnect()) {
        stop_flag.store(true);
        owner_thread.join();
        result.failure_reason = "session_reconnect_requested";
        result.samples_ns.clear();
        return result;
      }
      std::this_thread::yield();
    }
  }

  stop_flag.store(true);
  owner_thread.join();
  if (session.ShouldReconnect()) {
    result.failure_reason = "session_reconnect_requested";
    result.samples_ns.clear();
    return result;
  }

  result.rx_messages = metrics.rx_messages;
  result.success = true;
  return result;
}

void PrintVariantFailure(const VariantSpec& variant,
                         const VariantResult& result) {
  PrintReport(variant.name, {}, false, "local-socketpair", "messages", 0);
  std::printf(
      "name=%.*s status=skipped reason=%s affinity_mode=%u io_cpu_id=%d "
      "lock_memory=%s prefault_stack=%s\n",
      static_cast<int>(variant.name.size()), variant.name.data(),
      result.failure_reason == nullptr ? "unknown" : result.failure_reason,
      static_cast<unsigned>(variant.runtime_policy.affinity_mode),
      variant.runtime_policy.io_cpu_id,
      variant.runtime_policy.lock_memory ? "true" : "false",
      variant.runtime_policy.prefault_stack ? "true" : "false");
}

}  // namespace

int main() {
  int first_cpu = -1;
  const bool has_cpu = FirstAvailableCpu(&first_cpu);

  RuntimePolicy baseline_policy{};
  baseline_policy.affinity_mode = AffinityMode::kNone;
  baseline_policy.io_cpu_id = -1;
  baseline_policy.lock_memory = false;
  baseline_policy.prefault_stack = false;

  RuntimePolicy pinned_policy = baseline_policy;
  pinned_policy.affinity_mode = AffinityMode::kBestEffort;
  pinned_policy.io_cpu_id = has_cpu ? first_cpu : -1;

  RuntimePolicy pinned_prefault_policy = pinned_policy;
  pinned_prefault_policy.prefault_stack = true;

  RuntimePolicy pinned_locked_policy = pinned_prefault_policy;
  pinned_locked_policy.lock_memory = true;

  const std::vector<VariantSpec> variants = {
      {"affinity_policy_comparison/baseline", baseline_policy},
      {"affinity_policy_comparison/pinned", pinned_policy},
      {"affinity_policy_comparison/pinned_prefault", pinned_prefault_policy},
      {"affinity_policy_comparison/pinned_locked", pinned_locked_policy},
  };

  for (const VariantSpec& variant : variants) {
    if (variant.runtime_policy.affinity_mode != AffinityMode::kNone &&
        variant.runtime_policy.io_cpu_id < 0) {
      VariantResult skipped;
      skipped.failure_reason = "no_available_cpu";
      PrintVariantFailure(variant, skipped);
      continue;
    }

    VariantResult result = RunVariant(variant);
    if (!result.success) {
      PrintVariantFailure(variant, result);
      continue;
    }

    PrintReport(variant.name, std::move(result.samples_ns), false,
                "local-socketpair", "messages", result.rx_messages);
  }

  return 0;
}
