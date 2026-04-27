#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/cold_path_loop.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/state_machine.h"
#include "core/websocket/tls_socket.h"
#include "core/websocket/types.h"
#include "test/websocket/tls_blackhole_server.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/compile.h>
#include <fmt/format.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

void BenchmarkSessionTlsWritePath(benchmark::State& state) {
  aquila::websocket::test::TlsBlackholeServer server({
      aquila::websocket::test::TlsServerAction::kHandshake101ThenDrain,
  });
  if (!server.Start()) {
    state.SkipWithError("local TLS drain server start failed");
    return;
  }

  ConnectionConfig config{};
  config.host = "localhost";
  config.service = fmt::format(FMT_COMPILE("{}"), server.port());
  config.target = "/tls-write-path";
  config.enable_tls = true;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 128;
  config.frame_buffer_bytes = 4096;
  config.heartbeat_interval_ms = std::numeric_limits<std::uint32_t>::max();
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();
  config.runtime_policy.affinity_mode = AffinityMode::kNone;
  config.runtime_policy.lock_memory = false;
  config.runtime_policy.prefault_stack = false;

  TlsSocket socket;
  StateMachine state_machine;
  ColdPathLoop cold_path_loop;
  std::array<char, 4096> handshake_storage{};
  if (!cold_path_loop.RunUntilActive(socket, state_machine, config,
                                     handshake_storage)) {
    state.SkipWithError("cold path failed before active state");
    return;
  }
  if (state_machine.phase() != ConnectionPhase::kActive ||
      state_machine.last_error() != ConnectionError::kNone) {
    state.SkipWithError("cold path did not enter active state");
    return;
  }

  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  CriticalSession<TlsSocket> session(config, socket, arena, metrics);

  const auto payload = BuildWritePayload();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

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
  }

  socket.Close();
  SetLatencyCounters(state, std::move(samples_ns), "tx_messages",
                     metrics.tx_messages);
  state.counters["drained_bytes"] =
      static_cast<double>(server.drained_bytes());
  state.SetLabel(BuildBenchmarkLabel(true, "local-tls-drain",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkSessionTlsWritePath)
    ->Name("session_tls_write_path")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
