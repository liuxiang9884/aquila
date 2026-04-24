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
#include <utility>
#include <vector>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

struct RuntimeContext {
  std::atomic<size_t> delivered{0};
  std::atomic<std::uint64_t> send_ns{0};
  std::vector<std::uint64_t>* samples_ns{nullptr};
};

DeliveryResult RecordLoopback(void* context, const MessageView&) noexcept {
  auto* runtime = static_cast<RuntimeContext*>(context);
  const size_t index = runtime->delivered.fetch_add(1);
  if (runtime->samples_ns == nullptr || index >= runtime->samples_ns->size()) {
    return DeliveryResult::kFatal;
  }
  (*runtime->samples_ns)[index] = NowNs() - runtime->send_ns.load();
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

}  // namespace

int main() {
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
    return 1;
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
  RuntimeSession runtime_session{session, stop_flag};
  ActiveSpinLoop loop(config.runtime_policy);
  std::thread owner_thread([&loop, &runtime_session]() { loop.Run(runtime_session); });

  const auto frame = BuildServerTextFrame("tick");
  for (size_t sample_index = 0; sample_index < kSamples; ++sample_index) {
    runtime_context.send_ns.store(NowNs());
    if (!WriteAllFd(pair.peer_fd, frame)) {
      stop_flag.store(true);
      owner_thread.join();
      return 1;
    }
    while (runtime_context.delivered.load() <= sample_index) {
      if (session.ShouldReconnect()) {
        stop_flag.store(true);
        owner_thread.join();
        return 1;
      }
      std::this_thread::yield();
    }
  }

  stop_flag.store(true);
  owner_thread.join();
  if (session.ShouldReconnect()) {
    return 1;
  }

  PrintReport("runtime_loopback", std::move(samples_ns), false,
              "local-socketpair", "messages", metrics.rx_messages);
  return 0;
}
