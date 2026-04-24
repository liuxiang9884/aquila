#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/types.h"

#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

struct ReadContext {
  std::uint64_t messages{0};
  size_t bytes{0};
};

DeliveryResult RecordRead(void* context, const MessageView& view) noexcept {
  auto* state = static_cast<ReadContext*>(context);
  ++state->messages;
  state->bytes += view.payload.size();
  return DeliveryResult::kAccepted;
}

void BenchmarkSessionReadPath(benchmark::State& state) {
  ConnectionConfig config{};
  config.enable_tls = false;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 128;
  config.read_buffer_bytes = 4096;
  config.frame_buffer_bytes = 4096;
  config.heartbeat_interval_ms = std::numeric_limits<std::uint32_t>::max();
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();

  SocketPair pair;
  if (!CreateSocketPair(pair)) {
    state.SkipWithError("socketpair create failed");
    return;
  }

  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  ReadContext read_context{};
  MessageConsumer consumer{&read_context, &RecordRead};
  CriticalSession<LocalFdSocket> session(config, pair.client, arena, metrics);
  session.SetConsumer(consumer);

  const auto frame = BuildServerTextFrame("market-data");
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    state.PauseTiming();
    const std::uint64_t previous_messages = read_context.messages;
    if (!WriteAllFd(pair.peer_fd, frame)) {
      state.SkipWithError("peer write failed");
      return;
    }
    state.ResumeTiming();

    const std::uint64_t start_ns = NowNs();
    while (read_context.messages == previous_messages) {
      session.DriveRead();
      if (session.ShouldReconnect()) {
        state.SkipWithError("session requested reconnect");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
  }

  SetLatencyCounters(state, std::move(samples_ns), "rx_messages",
                     metrics.rx_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "local-socketpair",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkSessionReadPath)
    ->Name("session_read_path")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
