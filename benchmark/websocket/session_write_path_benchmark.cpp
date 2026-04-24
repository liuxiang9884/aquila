#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

void BenchmarkSessionWritePath(benchmark::State& state) {
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
  CriticalSession<LocalFdSocket> session(config, pair.client, arena, metrics);
  const auto payload = BuildWritePayload();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::array<std::byte, 64> peer_drain{};

  for (auto _ : state) {
    state.PauseTiming();
    PreparedWrite* write = session.TryAcquirePreparedWrite();
    if (write == nullptr) {
      state.SkipWithError("prepared write slot unavailable");
      return;
    }
    std::copy(payload.begin(), payload.end(), write->storage.begin());
    write->encoded_size = static_cast<std::uint32_t>(payload.size());
    write->kind = PayloadKind::kBinary;
    state.ResumeTiming();

    const std::uint64_t start_ns = NowNs();
    if (session.CommitPreparedWrite(write) != SendStatus::kOk) {
      state.SkipWithError("commit prepared write failed");
      return;
    }
    while (session.WantsWrite()) {
      session.DriveWrite();
      if (session.ShouldReconnect()) {
        state.SkipWithError("session requested reconnect");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);

    state.PauseTiming();
    if (!ReadExactFd(pair.peer_fd, peer_drain)) {
      state.SkipWithError("peer drain failed");
      return;
    }
    state.ResumeTiming();
  }

  SetLatencyCounters(state, std::move(samples_ns), "tx_messages",
                     metrics.tx_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "local-socketpair",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkSessionWritePath)
    ->Name("session_write_path")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
